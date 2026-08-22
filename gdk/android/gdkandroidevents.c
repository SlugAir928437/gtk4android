/*
 * Copyright (c) 2024-2025 Florian "sp1rit" <sp1rit@disroot.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "gdkeventsprivate.h"

#include "gdkandroidinit-private.h"
#include "gdkandroiddisplay-private.h"
#include "gdkandroidseat-private.h"
#include "gdkandroiddevice-private.h"

#include "gdkandroidtoplevel.h"

#include "gdkandroidevents-private.h"

/* Android platform constants. These mirror the values of the NDK's
 * <android/input.h> and the android.view.* framework classes, which are
 * identical for the subset of events handled here.
 *
 * We intentionally do NOT use the NDK native input API (libandroid's
 * AMotionEvent_* / AKeyEvent_* / AInputEvent_* family): its
 * AMotionEvent_fromJava/AKeyEvent_fromJava/AInputEvent_release entry
 * points are only provided on Android 12 (API 31) and above, so linking
 * against them makes libgtk-4.so unloadable on older devices (the dlopen
 * call fails with an UnsatisfiedLinkError). Instead, all event data is
 * read directly from the Java MotionEvent/KeyEvent objects via JNI. */

/* AMetaState flags (android.view.KeyEvent / AMETA_*) */
#define GDK_ANDROID_AMETA_SHIFT_ON     0x00000001
#define GDK_ANDROID_AMETA_CAPS_LOCK_ON 0x00100000
#define GDK_ANDROID_AMETA_CTRL_ON      0x00001000
#define GDK_ANDROID_AMETA_ALT_ON       0x00000002
#define GDK_ANDROID_AMETA_META_ON      0x00010000

/* Input sources (android.view.InputDevice / AINPUT_SOURCE_*) */
#define GDK_ANDROID_AINPUT_SOURCE_CLASS_POINTER 0x00000002
#define GDK_ANDROID_AINPUT_SOURCE_TOUCHSCREEN   0x00001002
#define GDK_ANDROID_AINPUT_SOURCE_JOYSTICK      0x00000400

/* Motion actions (android.view.MotionEvent / AMOTION_EVENT_ACTION_*) */
#define GDK_ANDROID_ACTION_DOWN           0
#define GDK_ANDROID_ACTION_UP             1
#define GDK_ANDROID_ACTION_MOVE           2
#define GDK_ANDROID_ACTION_CANCEL         3
#define GDK_ANDROID_ACTION_POINTER_DOWN   5
#define GDK_ANDROID_ACTION_POINTER_UP     6
#define GDK_ANDROID_ACTION_HOVER_MOVE     7
#define GDK_ANDROID_ACTION_SCROLL         8
#define GDK_ANDROID_ACTION_HOVER_ENTER    9
#define GDK_ANDROID_ACTION_HOVER_EXIT     10
#define GDK_ANDROID_ACTION_BUTTON_PRESS   11
#define GDK_ANDROID_ACTION_BUTTON_RELEASE 12

/* Motion buttons (android.view.MotionEvent / AMOTION_EVENT_BUTTON_*) */
#define GDK_ANDROID_BUTTON_PRIMARY          1
#define GDK_ANDROID_BUTTON_SECONDARY        2
#define GDK_ANDROID_BUTTON_TERTIARY         4
#define GDK_ANDROID_BUTTON_BACK             8
#define GDK_ANDROID_BUTTON_FORWARD          16
#define GDK_ANDROID_BUTTON_STYLUS_PRIMARY   32
#define GDK_ANDROID_BUTTON_STYLUS_SECONDARY 64

/* Motion tool types (android.view.MotionEvent / AMOTION_EVENT_TOOL_TYPE_*) */
#define GDK_ANDROID_TOOL_TYPE_FINGER  1
#define GDK_ANDROID_TOOL_TYPE_STYLUS  2
#define GDK_ANDROID_TOOL_TYPE_MOUSE   3
#define GDK_ANDROID_TOOL_TYPE_ERASER  4

/* Motion axes (android.view.MotionEvent / AMOTION_EVENT_AXIS_*) */
#define GDK_ANDROID_AXIS_HSCROLL 10
#define GDK_ANDROID_AXIS_VSCROLL 9
#define GDK_ANDROID_AXIS_WHEEL   21

/* Key actions (android.view.KeyEvent / AKEY_EVENT_ACTION_*) */
#define GDK_ANDROID_KEY_ACTION_DOWN 0
#define GDK_ANDROID_KEY_ACTION_UP   1

/* Key codes (android.view.KeyEvent / AKEYCODE_*) */
#define GDK_ANDROID_KEYCODE_BUTTON_1  188
#define GDK_ANDROID_KEYCODE_BUTTON_16 203

static GdkModifierType
gdk_android_events_meta_to_gdk (gint32 modifiers)
{
  GdkModifierType ret = 0;
  if (modifiers & GDK_ANDROID_AMETA_SHIFT_ON)
    ret |= GDK_SHIFT_MASK;
  if (modifiers & GDK_ANDROID_AMETA_CAPS_LOCK_ON)
    ret |= GDK_LOCK_MASK;
  if (modifiers & GDK_ANDROID_AMETA_CTRL_ON)
    ret |= GDK_CONTROL_MASK;
  if (modifiers & GDK_ANDROID_AMETA_ALT_ON)
    ret |= GDK_ALT_MASK;
  if (modifiers & GDK_ANDROID_AMETA_META_ON)
    ret |= GDK_META_MASK;
  return ret;
}

static GdkModifierType
gdk_android_events_buttons_to_gdkmods (gint32 buttons)
{
  GdkModifierType ret = 0;
  if (buttons & GDK_ANDROID_BUTTON_PRIMARY)
    ret |= GDK_BUTTON1_MASK;
  if (buttons & GDK_ANDROID_BUTTON_SECONDARY)
    ret |= GDK_BUTTON3_MASK; // X11 button numbering
  if (buttons & GDK_ANDROID_BUTTON_TERTIARY)
    ret |= GDK_BUTTON2_MASK; // ditto
  if (buttons & GDK_ANDROID_BUTTON_BACK)
    ret |= GDK_BUTTON4_MASK;
  if (buttons & GDK_ANDROID_BUTTON_FORWARD)
    ret |= GDK_BUTTON5_MASK;
  return ret;
}

static guint
gdk_android_surface_long_hash (guint64 num)
{
  return (gint) (num ^ (num >> 32));
}

// Taken from Thomas Mueller on SO, licensed under CC BY-SA 4.0
// https://stackoverflow.com/a/12996028/10890264
static guint
gdk_android_events_int_hash (guint32 num)
{
  num = ((num >> 16) ^ num) * 0x45d9f3bu;
  num = ((num >> 16) ^ num) * 0x45d9f3bu;
  num = (num >> 16) ^ num;
  return num;
}

#define GDK_ANDROID_EVENTS_COMPARE_MASK(val, mask) \
  (((val) & (mask)) == (mask))

#define GDK_ANDROID_EVENTS_BUTTON_IS_DIFFERENT(state, prev, mask) \
  (((state) & (mask)) ^ ((prev) & (mask)))

/* Thin JNI call helpers to keep the event handlers below readable. */
static jint
gdk_android_events_call_int (JNIEnv *env, jobject obj, jmethodID method)
{
  return (*env)->CallIntMethod (env, obj, method);
}

static jint
gdk_android_events_call_int_1 (JNIEnv *env, jobject obj, jmethodID method, jint arg)
{
  return (*env)->CallIntMethod (env, obj, method, arg);
}

static jlong
gdk_android_events_call_long (JNIEnv *env, jobject obj, jmethodID method)
{
  return (*env)->CallLongMethod (env, obj, method);
}

static jfloat
gdk_android_events_call_float_1 (JNIEnv *env, jobject obj, jmethodID method, jint arg)
{
  return (*env)->CallFloatMethod (env, obj, method, arg);
}

static jfloat
gdk_android_events_call_float_2 (JNIEnv *env, jobject obj, jmethodID method, jint arg0, jint arg1)
{
  return (*env)->CallFloatMethod (env, obj, method, arg0, arg1);
}

static GdkEventType
gdk_android_events_touch_action_to_gdk (jint masked_action, jint action_index, size_t pointer_index)
{
  if (masked_action == GDK_ANDROID_ACTION_POINTER_DOWN ||
      masked_action == GDK_ANDROID_ACTION_POINTER_UP)
    {
      if (pointer_index != (size_t) action_index)
        return GDK_TOUCH_UPDATE;
      return masked_action == GDK_ANDROID_ACTION_POINTER_DOWN ? GDK_TOUCH_BEGIN : GDK_TOUCH_END;
    }

  switch (masked_action)
    {
    case GDK_ANDROID_ACTION_DOWN:
      return GDK_TOUCH_BEGIN;
    case GDK_ANDROID_ACTION_UP:
      return GDK_TOUCH_END;
    case GDK_ANDROID_ACTION_MOVE:
      return GDK_TOUCH_UPDATE;
    case GDK_ANDROID_ACTION_CANCEL:
      return GDK_TOUCH_CANCEL;
    default:
      __builtin_unreachable ();
    }
}

static void
gdk_android_events_emit_button_press (gint32 mask, guint32 state, guint button,
                                      GdkAndroidSurface *surface,
                                      JNIEnv *env, jobject motion_event,
                                      gint32 tool_type, GdkDevice *dev,
                                      guint32 time, GdkModifierType mods,
                                      gdouble x, gdouble y)
{
  GdkDisplay *display = gdk_surface_get_display ((GdkSurface *) surface);
  GdkDeviceTool *tool = gdk_android_seat_get_device_tool (GDK_ANDROID_DISPLAY (display)->seat,
                                                          tool_type);
  g_debug ("Mouse %u event: (%d & %d) %p [%s]: %s", button, mask, state, surface, G_OBJECT_TYPE_NAME (surface), (state & mask) != 0 ? "press" : "release");
  GdkEvent *ev = gdk_button_event_new ((state & mask) != 0 ? GDK_BUTTON_PRESS : GDK_BUTTON_RELEASE,
                                       (GdkSurface *) surface, dev, tool,
                                       time, mods,
                                       button,
                                       x, y,
                                       gdk_android_seat_create_axes_from_motion_event (env, motion_event, 0));
  gdk_android_seat_consume_event (display, ev);
}

void
gdk_android_events_handle_motion_event (GdkAndroidSurface *surface,
                                        jobject            motion_event,
                                        jint               event_identifier)
{
  GdkAndroidDisplay *display = (GdkAndroidDisplay *) gdk_surface_get_display ((GdkSurface *) surface);

  JNIEnv *env = gdk_android_get_env ();
  const GdkAndroidJavaCache *cache = gdk_android_get_java_cache ();

  gint32 masked_action = gdk_android_events_call_int (env, motion_event, cache->motion_event.get_action_mask);
  gint32 src = gdk_android_events_call_int (env, motion_event, cache->a_input_event.get_source);

  GdkDevice *dev = gdk_seat_get_pointer ((GdkSeat *) display->seat);
  GdkAndroidDevice *dev_impl = GDK_ANDROID_DEVICE (dev);

  GdkModifierType mods = gdk_android_events_meta_to_gdk (gdk_android_events_call_int (env, motion_event, cache->motion_event.get_meta_state));
  mods |= gdk_android_events_buttons_to_gdkmods (dev_impl->button_state);

  gint64 time = gdk_android_events_call_long (env, motion_event, cache->motion_event.get_event_time);

  // Update keyboard focus on motion events only for autohide surfaces
  // This *doesn't really* match the behaviour of Mutter (autohide popups
  // get keyboard focus on present, while non-autohide popups do not),
  // especially as motion events shouldn't update keyboard focus, but it'll
  // work in the grand scheme of things for now.
  if (GDK_IS_ANDROID_TOPLEVEL(surface) || ((GdkSurface *)surface)->autohide)
    {
      GdkDevice *keyboard = gdk_seat_get_keyboard ((GdkSeat *) display->seat);
      gdk_android_device_keyboard_maybe_update_surface_focus ((GdkAndroidDevice *) keyboard, surface);
    }

  if (GDK_ANDROID_EVENTS_COMPARE_MASK (src, GDK_ANDROID_AINPUT_SOURCE_TOUCHSCREEN))
    {
      // I think it might be better to drop the down time and only rely on the event identity
      guint base_sequence = gdk_android_surface_long_hash ((guint64) gdk_android_events_call_long (env, motion_event, cache->motion_event.get_down_time));
      base_sequence ^= (guint) event_identifier;

      jint action_index = gdk_android_events_call_int (env, motion_event, cache->motion_event.get_action_index);
      size_t pointers = gdk_android_events_call_int (env, motion_event, cache->motion_event.get_pointer_count);
      for (size_t i = 0; i < pointers; i++)
        {
          GdkEventType ev_type = gdk_android_events_touch_action_to_gdk (masked_action, action_index, i);

          guint sequence = base_sequence ^ gdk_android_events_int_hash(gdk_android_events_call_int_1 (env, motion_event, cache->motion_event.get_pointer_id, (jint) i));
          gfloat x = gdk_android_events_call_float_1 (env, motion_event, cache->motion_event.get_x, (jint) i) / surface->cfg.scale;
          gfloat y = gdk_android_events_call_float_1 (env, motion_event, cache->motion_event.get_y, (jint) i) / surface->cfg.scale;

          GdkEvent *ev = gdk_touch_event_new (ev_type, GUINT_TO_POINTER (sequence), (GdkSurface *) surface,
                                              display->seat->logical_touchscreen,
                                              time, mods,
                                              x, y,
                                              gdk_android_seat_create_axes_from_motion_event (env, motion_event, i), i == 0);
          gdk_android_seat_consume_event ((GdkDisplay *) display, ev);
        }
    }
  else if (GDK_ANDROID_EVENTS_COMPARE_MASK (src, GDK_ANDROID_AINPUT_SOURCE_CLASS_POINTER))
    {
      gfloat x = gdk_android_events_call_float_1 (env, motion_event, cache->motion_event.get_x, 0) / surface->cfg.scale;
      gfloat y = gdk_android_events_call_float_1 (env, motion_event, cache->motion_event.get_y, 0) / surface->cfg.scale;

      if (masked_action == GDK_ANDROID_ACTION_SCROLL)
        {
          GdkEvent *ev = gdk_scroll_event_new ((GdkSurface *) surface, dev, NULL,
                                               time, mods,
                                               gdk_android_events_call_float_2 (env, motion_event, cache->motion_event.get_axis_value, GDK_ANDROID_AXIS_HSCROLL, 0),
                                               gdk_android_events_call_float_2 (env, motion_event, cache->motion_event.get_axis_value, GDK_ANDROID_AXIS_VSCROLL, 0),
                                               FALSE, // how am I supposed to know if the current scroll event is the last?
                                               GDK_SCROLL_UNIT_WHEEL,
                                               GDK_SCROLL_RELATIVE_DIRECTION_UNKNOWN);
          gdk_android_seat_consume_event ((GdkDisplay *) display, ev);
        }
      else if (masked_action == GDK_ANDROID_ACTION_DOWN ||
               masked_action == GDK_ANDROID_ACTION_UP ||
               masked_action == GDK_ANDROID_ACTION_CANCEL)
        { // we have to treat cancel like a
          // button up event, as GDK does not
          // provide a cancel mechanism for
          // button events.
          gint32 tool_type = gdk_android_events_call_int_1 (env, motion_event, cache->motion_event.get_tool_type, 0);
          if (tool_type == GDK_ANDROID_TOOL_TYPE_MOUSE || tool_type == GDK_ANDROID_TOOL_TYPE_FINGER)
            {
              gint32 button_state = gdk_android_events_call_int (env, motion_event, cache->motion_event.get_button_state);
              if (GDK_ANDROID_EVENTS_BUTTON_IS_DIFFERENT (button_state, dev_impl->button_state, GDK_ANDROID_BUTTON_PRIMARY))
                gdk_android_events_emit_button_press (GDK_ANDROID_BUTTON_PRIMARY, button_state,
                                                      GDK_BUTTON_PRIMARY,
                                                      surface, env, motion_event,
                                                      tool_type, dev,
                                                      time, mods,
                                                      x, y);
              if (GDK_ANDROID_EVENTS_BUTTON_IS_DIFFERENT (button_state, dev_impl->button_state, GDK_ANDROID_BUTTON_SECONDARY))
                gdk_android_events_emit_button_press (GDK_ANDROID_BUTTON_SECONDARY, button_state,
                                                      GDK_BUTTON_SECONDARY,
                                                      surface, env, motion_event,
                                                      tool_type, dev,
                                                      time, mods,
                                                      x, y);
              if (GDK_ANDROID_EVENTS_BUTTON_IS_DIFFERENT (button_state, dev_impl->button_state, GDK_ANDROID_BUTTON_TERTIARY))
                gdk_android_events_emit_button_press (GDK_ANDROID_BUTTON_TERTIARY, button_state,
                                                      GDK_BUTTON_MIDDLE,
                                                      surface, env, motion_event,
                                                      tool_type, dev,
                                                      time, mods,
                                                      x, y);

              const guint update_mask = GDK_ANDROID_BUTTON_PRIMARY | GDK_ANDROID_BUTTON_SECONDARY | GDK_ANDROID_BUTTON_TERTIARY;
              dev_impl->button_state = (dev_impl->button_state & ~update_mask) | (button_state & update_mask);
            }
          else if (tool_type == GDK_ANDROID_TOOL_TYPE_STYLUS || tool_type == GDK_ANDROID_TOOL_TYPE_ERASER)
            {
              GdkDeviceTool *tool = gdk_android_seat_get_device_tool (GDK_ANDROID_DISPLAY (display)->seat, tool_type);
              GdkEvent *ev = gdk_button_event_new (masked_action == GDK_ANDROID_ACTION_DOWN ? GDK_BUTTON_PRESS : GDK_BUTTON_RELEASE,
                                                   (GdkSurface *) surface, dev, tool,
                                                   time, mods,
                                                   GDK_BUTTON_PRIMARY,
                                                   x, y,
                                                   gdk_android_seat_create_axes_from_motion_event (env, motion_event, 0));
              gdk_android_seat_consume_event ((GdkDisplay *) display, ev);

              // this will cause conflicts in cases where the mouse/touchpad and
              // a stylus is used at the same time, but I don't know if this is
              // worth handling
              if (masked_action == GDK_ANDROID_ACTION_DOWN)
                dev_impl->button_state |= GDK_ANDROID_BUTTON_PRIMARY;
              else
                dev_impl->button_state &= ~GDK_ANDROID_BUTTON_PRIMARY;
            }
          gdk_android_device_maybe_update_surface ((GdkAndroidDevice *) dev, surface, mods, time, x, y);
        }
      else if (masked_action == GDK_ANDROID_ACTION_BUTTON_PRESS ||
               masked_action == GDK_ANDROID_ACTION_BUTTON_RELEASE)
        {
          // This code serves little purpose, as BUTTON_BACK and BUTTON_FORWARD are
          // are seemingly not actually passed to the application. Instead
          // BUTTON_BACK triggers the navigate back action and BUTTON_FORWARD does
          // (at least visibly) nothing.
          /*gint32 button_state = ...getButtonState();

          if (GDK_ANDROID_EVENTS_BUTTON_IS_DIFFERENT(button_state, dev_impl->button_state, GDK_ANDROID_BUTTON_BACK))
                  gdk_android_events_emit_button_press(GDK_ANDROID_BUTTON_BACK, 4, button_state, surface, env, motion_event, tool_type, dev, time, mods | GDK_BUTTON4_MASK, x, y);
          if (GDK_ANDROID_EVENTS_BUTTON_IS_DIFFERENT(button_state, dev_impl->button_state, GDK_ANDROID_BUTTON_FORWARD))
                  gdk_android_events_emit_button_press(GDK_ANDROID_BUTTON_FORWARD, 5, button_state, surface, env, motion_event, tool_type, dev, time, mods | GDK_BUTTON5_MASK, x, y);

          const guint update_mask = GDK_ANDROID_BUTTON_BACK | GDK_ANDROID_BUTTON_FORWARD;
          dev_impl->button_state = (dev_impl->button_state & ~update_mask) | (button_state & update_mask);*/
        }
      else if (masked_action == GDK_ANDROID_ACTION_MOVE ||
               masked_action == GDK_ANDROID_ACTION_HOVER_MOVE)
        {
          GdkDeviceTool *tool = gdk_android_seat_get_device_tool (display->seat,
                                                                  gdk_android_events_call_int_1 (env, motion_event, cache->motion_event.get_tool_type, 0));
          GdkEvent *ev = gdk_motion_event_new ((GdkSurface *) surface, dev, tool,
                                               time, mods,
                                               x, y,
                                               gdk_android_seat_create_axes_from_motion_event (env, motion_event, 0));
          gdk_android_seat_consume_event ((GdkDisplay *) display, ev);

          // as changes in BUTTON_STYLUS_{PRIMARY,SECONDARY} do not emit a special
          // event, we'll have to check for changes during move events. This should
          // be fine, given that it's quite hard if not impossible (depending on the
          // tablet) to press a stylus button without causing a move event.
          gint32 button_state = gdk_android_events_call_int (env, motion_event, cache->motion_event.get_button_state);
          if (GDK_ANDROID_EVENTS_BUTTON_IS_DIFFERENT (button_state, dev_impl->button_state, GDK_ANDROID_BUTTON_STYLUS_PRIMARY))
            gdk_android_events_emit_button_press (GDK_ANDROID_BUTTON_STYLUS_PRIMARY, button_state,
                                                  GDK_BUTTON_MIDDLE,
                                                  surface, env, motion_event,
                                                  GDK_ANDROID_TOOL_TYPE_STYLUS, dev,
                                                  time, mods,
                                                  x, y);
          if (GDK_ANDROID_EVENTS_BUTTON_IS_DIFFERENT (button_state, dev_impl->button_state, GDK_ANDROID_BUTTON_STYLUS_SECONDARY))
            gdk_android_events_emit_button_press (GDK_ANDROID_BUTTON_STYLUS_SECONDARY, button_state,
                                                  GDK_BUTTON_SECONDARY,
                                                  surface, env, motion_event,
                                                  GDK_ANDROID_TOOL_TYPE_STYLUS, dev,
                                                  time, mods,
                                                  x, y);
          const guint update_mask = GDK_ANDROID_BUTTON_STYLUS_PRIMARY | GDK_ANDROID_BUTTON_STYLUS_SECONDARY;
          dev_impl->button_state = (dev_impl->button_state & ~update_mask) | (button_state & update_mask);

          gdk_android_device_maybe_update_surface ((GdkAndroidDevice *) dev, surface, mods, time, x, y);
        }
      else if (masked_action == GDK_ANDROID_ACTION_HOVER_ENTER ||
               masked_action == GDK_ANDROID_ACTION_HOVER_EXIT)
        {
          // This would be a good place to put crossing events, however it seems like android also
          // produces hover enter/hover exit events when clicking the button.
          /*GdkEvent *ev = gdk_crossing_event_new ((masked_action == GDK_ANDROID_ACTION_HOVER_ENTER) ? GDK_ENTER_NOTIFY : GDK_LEAVE_NOTIFY,
                                                                                     (GdkSurface*) surface, dev,
                                                                                     time, mods,
                                                                                     x, y,
                                                                                     GDK_CROSSING_NORMAL,
                                                                                     GDK_NOTIFY_UNKNOWN);
          gdk_android_seat_consume_event((GdkDisplay *)display, ev);*/
        }
      else
        {
          g_warning ("Unhandled pointer event: %d [%d] on %p [%s]", masked_action, src, surface, G_OBJECT_TYPE_NAME (surface));
        }
    }
  else if (GDK_ANDROID_EVENTS_COMPARE_MASK (src, GDK_ANDROID_AINPUT_SOURCE_JOYSTICK))
    {
      (*env)->PushLocalFrame (env, 2);
      jobject jdevice = (*env)->CallObjectMethod (env, motion_event, cache->a_input_event.get_device);
      struct
      {
        gint32 axis;
        guint index;
        gfloat min;
        gfloat max;
        GdkEvent *(*constructor) (GdkSurface *surface, GdkDevice *device, guint32 time, guint group, guint index, guint mode, double value);
      } pad_axes[] = {
        { GDK_ANDROID_AXIS_WHEEL, 0, 0.f, 360.f, gdk_pad_event_new_ring }
      };
      for (gsize i = 0; i < G_N_ELEMENTS (pad_axes); i++)
        {
          gdouble value;
          if (gdk_android_seat_normalize_range (env, jdevice, motion_event, 0, pad_axes[i].axis, pad_axes[i].min, pad_axes[i].max, &value) && value != 0.)
            { // the value != 0. check is less than ideal, as 0 is a legitimate value, but
              // android also returns 0 when the finger leaves the ring (and often just
              // randomly too)
              GdkEvent *ev = pad_axes[i].constructor ((GdkSurface *) surface, gdk_seat_get_keyboard ((GdkSeat *) display->seat),
                                                      time,
                                                      0, pad_axes[i].index, 0,
                                                      value);
              gdk_android_seat_consume_event ((GdkDisplay *) display, ev);
            }
        }
      (*env)->PopLocalFrame (env, NULL);
    }
}

void
gdk_android_events_handle_key_event (GdkAndroidSurface *surface,
                                     jobject            key_event)
{
  GdkAndroidDisplay *display = (GdkAndroidDisplay *) gdk_surface_get_display ((GdkSurface *) surface);

  JNIEnv *env = gdk_android_get_env ();
  const GdkAndroidJavaCache *cache = gdk_android_get_java_cache ();

  gint32 action = gdk_android_events_call_int (env, key_event, cache->key_event.get_action);
  // When the key is depressed the action is ACTION_DOWN (0), while the
  // action becomes ACTION_UP (1) once the key was released again.
  GdkEventType event_type = (action == GDK_ANDROID_KEY_ACTION_DOWN) ? GDK_KEY_PRESS : GDK_KEY_RELEASE;

  GdkDevice *dev = gdk_seat_get_keyboard ((GdkSeat *) display->seat);
  gdk_android_device_keyboard_maybe_update_surface_focus ((GdkAndroidDevice *) dev, surface);

  GdkModifierType mods = gdk_android_events_meta_to_gdk (gdk_android_events_call_int (env, key_event, cache->key_event.get_meta_state));
  mods |= gdk_android_events_buttons_to_gdkmods (((GdkAndroidDevice *) display->seat->logical_pointer)->button_state);

  /* android.view.KeyEvent.getEventTime() already returns milliseconds,
   * which is what GDK event timestamps expect. */
  gint64 time = gdk_android_events_call_long (env, key_event, cache->key_event.get_event_time);

  gint32 keycode = gdk_android_events_call_int (env, key_event, cache->key_event.get_key_code);

  if (keycode >= GDK_ANDROID_KEYCODE_BUTTON_1 && keycode <= GDK_ANDROID_KEYCODE_BUTTON_16)
    {
      // Key Event might be a Pad Button
      GdkEvent *ev = gdk_pad_event_new_button (event_type == GDK_KEY_PRESS ? GDK_PAD_BUTTON_PRESS : GDK_PAD_BUTTON_RELEASE,
                                               (GdkSurface *) surface, dev,
                                               time,
                                               0, keycode - GDK_ANDROID_KEYCODE_BUTTON_1, 0);
      gdk_android_seat_consume_event ((GdkDisplay *) display, ev);
      return;
    }

  GdkTranslatedKey translated;
  if (!gdk_keymap_translate_keyboard_state (display->keymap, keycode, mods, 0,
                                            &translated.keyval, (gint *) &translated.layout,
                                            (gint *) &translated.level, &translated.consumed))
    return;

  // TODO: do no_caps translation properly

  GdkEvent *ev = gdk_key_event_new (event_type,
                                    (GdkSurface *) surface, dev,
                                    time, keycode,
                                    mods & ~translated.consumed, FALSE,
                                    &translated, &translated, NULL);
  gdk_android_seat_consume_event ((GdkDisplay *) display, ev);
}
