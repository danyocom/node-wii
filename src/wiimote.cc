/*
 * Copyright 2011, Tim Branyen @tbranyen <tim@tabdeveloper.com>
 * Dual licensed under the MIT and GPL licenses.
 */

#include <v8.h>
#include <node.h>
#include <node_events.h>

#include <bluetooth/bluetooth.h>
#include "../vendor/cwiid/libcwiid/cwiid.h"

#include "../include/wiimote.h"

using namespace v8;
using namespace node;

cwiid_mesg_callback_t MsgCallback;

void WiiMote::Initialize (Handle<v8::Object> target) {
  HandleScope scope;

  Local<FunctionTemplate> t = FunctionTemplate::New(New);
  t->Inherit(EventEmitter::constructor_template);

  ir_event = NODE_PSYMBOL("ir");
  acc_event = NODE_PSYMBOL("acc");
  
  constructor_template = Persistent<FunctionTemplate>::New(t);
  constructor_template->InstanceTemplate()->SetInternalFieldCount(1);
  constructor_template->SetClassName(String::NewSymbol("WiiMote"));

  NODE_SET_PROTOTYPE_METHOD(constructor_template, "connect", Connect);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "rumble", Rumble);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "led", Led);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "ir", IrReporting);

  target->Set(String::NewSymbol("WiiMote"), constructor_template->GetFunction());
}

int WiiMote::Connect(const char* mac) {
  str2ba(mac, &this->mac);

  if(!(this->wiimote = cwiid_open(&this->mac, 0))) {
    return -1;
  }

  return 0;
}

int WiiMote::Rumble(bool on) {
  unsigned char rumble = on ? 1 : 0;

  if(cwiid_set_rumble(this->wiimote, rumble)) {
    return -1;
  }
  
  return 0;
}

int WiiMote::Led(int index, bool on) {
  int indexes[] = { CWIID_LED1_ON, CWIID_LED2_ON, CWIID_LED3_ON, CWIID_LED4_ON };

  cwiid_get_state(this->wiimote, &this->state);

  int led = this->state.led;

  led = on ? led | indexes[index-1] : led & indexes[index-1];

  if(cwiid_set_led(this->wiimote, led)) {
    return -1;
  }

  return 0;
}

int WiiMote::WatchMessages() {
  //this->Ref();
  //cwiid_set_data(this->wiimote, this);

  //if(cwiid_set_mesg_callback(this->wiimote, MsgCallback)) {
  //  return -1;
  //}

  ev_timer_init(&this->msg_timer, TriggerMessages, 0., 1.);
  this->msg_timer.data=this;

  ev_timer_start(&this->msg_timer);

  return 0;
}

int WiiMote::IrReporting(bool on) {
  int mode = this->state.rpt_mode;

  mode = on ? mode | CWIID_RPT_IR : mode & CWIID_RPT_IR;

  if(cwiid_set_rpt_mode(this->wiimote, mode)) {
    return -1;
  }
}

void WiiMote::TriggerMessages(EV_P_ ev_timer *watcher, int revents) {
  WiiMote *wiimote = static_cast<WiiMote*>(watcher->data);

  HandleScope scope;

  Local<Value> argv[2];

  if(cwiid_get_state(wiimote->wiimote, &wiimote->state)) {
    argv[0] = Integer::New(-1);
    wiimote->Emit(ir_event, 1, argv);
  }

  bool valid = false;
	for(int i=0; i < CWIID_IR_SRC_COUNT; i++) {
    valid = true;
    argv[0] = Integer::New(0);

    // Create array of x,y
    Local<Array> pos = Array::New(2);
    pos->Set(Number::New(0), Integer::New(wiimote->state.ir_src[i].pos[CWIID_X]));
    pos->Set(Number::New(1), Integer::New(wiimote->state.ir_src[i].pos[CWIID_Y]));

    argv[1] = pos;
    wiimote->Emit(ir_event, 2, argv);
  }

  if(!valid) {
    argv[0] = Integer::New(-1);
    wiimote->Emit(ir_event, 1, argv);
  }

  ev_timer_stop(&wiimote->msg_timer);
  ev_timer_set(&wiimote->msg_timer, 0., 1.);
  ev_timer_start(&wiimote->msg_timer);

  if(wiimote->msg_timer.repeat == 0) {
    printf("%s", "x2");
    ev_timer_stop(&wiimote->msg_timer);
    wiimote->Unref();
  }
}

//void WiiMote::MsgCallback(cwiid_wiimote_t* _wiimote, int msg_count, union cwiid_mesg msg[], struct timespec *timestamp) {
//  printf("%s", "hit");
//  WiiMote* wiimote = (WiiMote*)cwiid_get_data(_wiimote);
//
//  HandleScope scope;
//
//
//}

Handle<Value> WiiMote::New(const Arguments& args) {
  HandleScope scope;

  WiiMote* wiimote = new WiiMote();
  wiimote->Wrap(args.This());

  return args.This();
}

Handle<Value> WiiMote::Connect(const Arguments& args) {
  WiiMote* wiimote = ObjectWrap::Unwrap<WiiMote>(args.This());
  Local<Function> callback;

  HandleScope scope;

  if(args.Length() == 0 || !args[0]->IsString()) {
    return ThrowException(Exception::Error(String::New("MAC address is required and must be a String.")));
  }

  if(args.Length() == 1 || !args[1]->IsFunction()) {
    return ThrowException(Exception::Error(String::New("Callback is required and must be a Function.")));
  }

  callback = Local<Function>::Cast(args[1]);

  connect_request* ar = new connect_request();
  ar->wiimote = wiimote;

  String::Utf8Value mac(args[0]);
  ar->mac = *mac;
  ar->callback = Persistent<Function>::New(callback);

  wiimote->Ref();

  eio_custom(EIO_Connect, EIO_PRI_DEFAULT, EIO_AfterConnect, ar);
  ev_ref(EV_DEFAULT_UC);

  return Undefined();
}

int WiiMote::EIO_Connect(eio_req* req) {
  connect_request* ar = static_cast<connect_request* >(req->data);

  ar->err = ar->wiimote->Connect(ar->mac);

  return 0;
}

int WiiMote::EIO_AfterConnect(eio_req* req) {
  HandleScope scope;

  connect_request* ar = static_cast<connect_request* >(req->data);
  ev_unref(EV_DEFAULT_UC);


  Local<Value> argv[1];
  argv[0] = Integer::New(ar->err);

  if(ar->wiimote->WatchMessages()) {
    argv[0] = Integer::New(-1);   
  }

  ar->wiimote->Unref();

  TryCatch try_catch;

  ar->callback->Call(Context::GetCurrent()->Global(), 1, argv);

  if(try_catch.HasCaught())
    FatalException(try_catch);
    
  ar->callback.Dispose();

  delete ar;


  return 0;
}

Handle<Value> WiiMote::Rumble(const Arguments& args) {
  HandleScope scope;

  WiiMote* wiimote = ObjectWrap::Unwrap<WiiMote>(args.This());

  if(args.Length() == 0 || !args[0]->IsBoolean()) {
    return ThrowException(Exception::Error(String::New("On state is required and must be a Boolean.")));
  }

  bool on = args[0]->ToBoolean()->Value();

  return Integer::New(wiimote->Rumble(on));
}

Handle<Value> WiiMote::Led(const Arguments& args) {
  HandleScope scope;

  WiiMote* wiimote = ObjectWrap::Unwrap<WiiMote>(args.This());

  if(args.Length() == 0 || !args[0]->IsNumber()) {
    return ThrowException(Exception::Error(String::New("Index is required and must be a Number.")));
  }

  if(args.Length() == 1 || !args[1]->IsBoolean()) {
    return ThrowException(Exception::Error(String::New("On state is required and must be a Boolean.")));
  }

  int index = args[0]->ToInteger()->Value();
  bool on = args[1]->ToBoolean()->Value();

  return Integer::New(wiimote->Led(index, on));
}

Handle<Value> WiiMote::IrReporting(const Arguments& args) {
  HandleScope scope;

  WiiMote* wiimote = ObjectWrap::Unwrap<WiiMote>(args.This());

  if(args.Length() == 0 || !args[0]->IsBoolean()) {
    return ThrowException(Exception::Error(String::New("On state is required and must be a Boolean.")));
  }

  bool on = args[0]->ToBoolean()->Value();

  return Integer::New(wiimote->IrReporting(on));
}

Persistent<FunctionTemplate> WiiMote::constructor_template;
Persistent<String> WiiMote::ir_event;
Persistent<String> WiiMote::acc_event;
