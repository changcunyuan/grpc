/*
 *
 * Copyright 2014, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "rb_grpc.h"

#include <math.h>
#include <ruby.h>
#include <sys/time.h>

#include <grpc/grpc.h>
#include <grpc/support/time.h>
#include "rb_byte_buffer.h"
#include "rb_call.h"
#include "rb_channel.h"
#include "rb_completion_queue.h"
#include "rb_event.h"
#include "rb_metadata.h"
#include "rb_server.h"
#include "rb_status.h"

/* Define common vars and funcs declared in rb.h */
const RUBY_DATA_FUNC GC_NOT_MARKED = NULL;
const RUBY_DATA_FUNC GC_DONT_FREE = NULL;

VALUE rb_cTimeVal = Qnil;

/* Alloc func that blocks allocation of a given object by raising an
 * exception. */
VALUE grpc_rb_cannot_alloc(VALUE cls) {
  rb_raise(rb_eTypeError,
           "allocation of %s only allowed from the gRPC native layer",
           rb_class2name(cls));
  return Qnil;
}

/* Init func that fails by raising an exception. */
VALUE grpc_rb_cannot_init(VALUE self) {
  rb_raise(rb_eTypeError,
           "initialization of %s only allowed from the gRPC native layer",
           rb_obj_classname(self));
  return Qnil;
}

/* Init/Clone func that fails by raising an exception. */
VALUE grpc_rb_cannot_init_copy(VALUE copy, VALUE self) {
  rb_raise(rb_eTypeError,
           "initialization of %s only allowed from the gRPC native layer",
           rb_obj_classname(copy));
  return Qnil;
}

/* id_tv_{,u}sec are accessor methods on Ruby Time instances. */
static ID id_tv_sec;
static ID id_tv_nsec;

/**
 * grpc_rb_time_timeval creates a time_eval from a ruby time object.
 *
 * This func is copied from ruby source, MRI/source/time.c, which is published
 * under the same license as the ruby.h, on which the entire extensions is
 * based.
 */
gpr_timespec grpc_rb_time_timeval(VALUE time, int interval) {
  gpr_timespec t;
  gpr_timespec *time_const;
  const char *tstr = interval ? "time interval" : "time";
  const char *want = " want <secs from epoch>|<Time>|<GRPC::TimeConst.*>";

  switch (TYPE(time)) {

    case T_DATA:
      if (CLASS_OF(time) == rb_cTimeVal) {
        Data_Get_Struct(time, gpr_timespec, time_const);
        t = *time_const;
      } else if (CLASS_OF(time) == rb_cTime) {
        t.tv_sec =  NUM2INT(rb_funcall(time, id_tv_sec, 0));
        t.tv_nsec = NUM2INT(rb_funcall(time, id_tv_nsec, 0));
      } else {
        rb_raise(rb_eTypeError,
                 "bad input: (%s)->c_timeval, got <%s>,%s",
                 tstr, rb_obj_classname(time), want);
      }
      break;

    case T_FIXNUM:
      t.tv_sec = FIX2LONG(time);
      if (interval && t.tv_sec < 0)
        rb_raise(rb_eArgError, "%s must be positive", tstr);
      t.tv_nsec = 0;
      break;

    case T_FLOAT:
      if (interval && RFLOAT(time)->float_value < 0.0)
        rb_raise(rb_eArgError, "%s must be positive", tstr);
      else {
        double f, d;

        d = modf(RFLOAT(time)->float_value, &f);
        if (d < 0) {
          d += 1;
          f -= 1;
        }
        t.tv_sec = (time_t)f;
        if (f != t.tv_sec) {
          rb_raise(rb_eRangeError, "%f out of Time range",
                   RFLOAT(time)->float_value);
        }
        t.tv_nsec = (time_t)(d*1e9+0.5);
      }
      break;

    case T_BIGNUM:
      t.tv_sec = NUM2LONG(time);
      if (interval && t.tv_sec < 0)
        rb_raise(rb_eArgError, "%s must be positive", tstr);
      t.tv_nsec = 0;
      break;

    default:
      rb_raise(rb_eTypeError,
               "bad input: (%s)->c_timeval, got <%s>,%s",
               tstr, rb_obj_classname(time), want);
      break;
  }
  return t;
}

/* id_at is the constructor method of the ruby standard Time class. */
static ID id_at;

/* id_inspect is the inspect method found on various ruby objects. */
static ID id_inspect;

/* id_to_s is the to_s method found on various ruby objects. */
static ID id_to_s;

/* Converts `a wrapped time constant to a standard time. */
VALUE grpc_rb_time_val_to_time(VALUE self) {
  gpr_timespec *time_const = NULL;
  Data_Get_Struct(self, gpr_timespec, time_const);
  return rb_funcall(rb_cTime, id_at, 2, INT2NUM(time_const->tv_sec),
                    INT2NUM(time_const->tv_nsec));
}

/* Invokes inspect on the ctime version of the time val. */
VALUE grpc_rb_time_val_inspect(VALUE self) {
  return rb_funcall(grpc_rb_time_val_to_time(self), id_inspect, 0);
}

/* Invokes to_s on the ctime version of the time val. */
VALUE grpc_rb_time_val_to_s(VALUE self) {
  return rb_funcall(grpc_rb_time_val_to_time(self), id_to_s, 0);
}

/* Adds a module with constants that map to gpr's static timeval structs. */
void Init_google_time_consts() {
  VALUE rb_mTimeConsts = rb_define_module_under(rb_mGoogleRPC, "TimeConsts");
  rb_cTimeVal = rb_define_class_under(rb_mGoogleRPC, "TimeSpec", rb_cObject);
  rb_define_const(rb_mTimeConsts, "ZERO",
                  Data_Wrap_Struct(rb_cTimeVal, GC_NOT_MARKED,
                                   GC_DONT_FREE, (void *)&gpr_time_0));
  rb_define_const(rb_mTimeConsts, "INFINITE_FUTURE",
                  Data_Wrap_Struct(rb_cTimeVal, GC_NOT_MARKED,
                                   GC_DONT_FREE, (void *)&gpr_inf_future));
  rb_define_const(rb_mTimeConsts, "INFINITE_PAST",
                  Data_Wrap_Struct(rb_cTimeVal, GC_NOT_MARKED,
                                   GC_DONT_FREE, (void *)&gpr_inf_past));
  rb_define_method(rb_cTimeVal, "to_time", grpc_rb_time_val_to_time, 0);
  rb_define_method(rb_cTimeVal, "inspect", grpc_rb_time_val_inspect, 0);
  rb_define_method(rb_cTimeVal, "to_s", grpc_rb_time_val_to_s, 0);
  id_at = rb_intern("at");
  id_inspect = rb_intern("inspect");
  id_to_s = rb_intern("to_s");
  id_tv_sec = rb_intern("tv_sec");
  id_tv_nsec = rb_intern("tv_nsec");
}

void grpc_rb_shutdown(void *vm) {
  grpc_shutdown();
}

/* Initialize the Google RPC module. */
VALUE rb_mGoogle = Qnil;
VALUE rb_mGoogleRPC = Qnil;
void Init_grpc() {
  grpc_init();
  ruby_vm_at_exit(grpc_rb_shutdown);
  rb_mGoogle = rb_define_module("Google");
  rb_mGoogleRPC = rb_define_module_under(rb_mGoogle, "RPC");

  Init_google_rpc_byte_buffer();
  Init_google_rpc_event();
  Init_google_rpc_channel();
  Init_google_rpc_completion_queue();
  Init_google_rpc_call();
  Init_google_rpc_metadata();
  Init_google_rpc_server();
  Init_google_rpc_status();
  Init_google_time_consts();
}