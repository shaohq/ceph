// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "Cond.h"
#include "Mutex.h"
#include "Thread.h"
#include "Timer.h"

#include "config.h"
#include "include/Context.h"

#define DOUT_SUBSYS timer
#undef dout_prefix
#define dout_prefix *_dout << "timer(" << this << ")."

#include <sstream>
#include <signal.h>
#include <sys/time.h>
#include <math.h>


class SafeTimerThread : public Thread {
  SafeTimer *parent;
public:
  SafeTimerThread(SafeTimer *s) : parent(s) {}
  void *entry() {
    parent->timer_thread();
    return NULL;
  }
} *thread;



typedef std::multimap < utime_t, Context *> scheduled_map_t;
typedef std::map < Context*, scheduled_map_t::iterator > event_lookup_map_t;

SafeTimer::SafeTimer(Mutex &l)
  : lock(l),
    thread(NULL),
    stopping(false) 
{
}

SafeTimer::~SafeTimer()
{
  assert(thread == NULL);
}

void SafeTimer::init()
{
  dout(10) << "init" << dendl;
  thread = new SafeTimerThread(this);
  thread->create();
}

void SafeTimer::shutdown()
{
  dout(10) << "shutdown" << dendl;
  if (thread) {
    assert(lock.is_locked());
    cancel_all_events();
    stopping = true;
    cond.Signal();
    lock.Unlock();
    thread->join();
    lock.Lock();
    delete thread;
    thread = NULL;
  }
}

void SafeTimer::timer_thread()
{
  lock.Lock();
  dout(10) << "timer_thread starting" << dendl;
  while (!stopping) {
    utime_t now = g_clock.now();
    
    while (!schedule.empty()) {
      scheduled_map_t::iterator p = schedule.begin();

      // is the future now?
      if (p->first > now)
	break;

      Context *callback = p->second;
      events.erase(callback);
      schedule.erase(p);
      dout(10) << "timer_thread executing " << callback << dendl;
      
      callback->finish(0);
      delete callback;
    }

    dout(20) << "timer_thread going to sleep" << dendl;
    if (schedule.empty())
      cond.Wait(lock);
    else
      cond.WaitUntil(lock, schedule.begin()->first);
    dout(20) << "timer_thread awake" << dendl;
  }
  dout(10) << "timer_thread exiting" << dendl;
  lock.Unlock();
}

void SafeTimer::add_event_after(double seconds, Context *callback)
{
  assert(lock.is_locked());

  utime_t when = g_clock.now();
  when += seconds;
  add_event_at(when, callback);
}

void SafeTimer::add_event_at(utime_t when, Context *callback)
{
  assert(lock.is_locked());
  dout(10) << "add_event_at " << when << " -> " << callback << dendl;

  scheduled_map_t::value_type s_val(when, callback);
  scheduled_map_t::iterator i = schedule.insert(s_val);

  event_lookup_map_t::value_type e_val(callback, i);
  pair < event_lookup_map_t::iterator, bool > rval(events.insert(e_val));

  /* If you hit this, you tried to insert the same Context* twice. */
  assert(rval.second);

  /* If the event we have just inserted comes before everything else, we need to
   * adjust our timeout. */
  if (i == schedule.begin())
    cond.Signal();

}

bool SafeTimer::cancel_event(Context *callback)
{
  assert(lock.is_locked());
  
  std::map<Context*, std::multimap<utime_t, Context*>::iterator>::iterator p = events.find(callback);
  if (p == events.end()) {
    dout(10) << "cancel_event " << callback << " not found" << dendl;
    return false;
  }

  dout(10) << "cancel_event " << p->second->first << " -> " << callback << dendl;
  delete p->first;

  schedule.erase(p->second);
  events.erase(p);
  return true;
}

void SafeTimer::cancel_all_events()
{
  dout(10) << "cancel_all_events" << dendl;
  assert(lock.is_locked());
  
  while (!events.empty()) {
    std::map<Context*, std::multimap<utime_t, Context*>::iterator>::iterator p = events.begin();
    dout(10) << " cancelled " << p->second->first << " -> " << p->first << dendl;
    delete p->first;
    schedule.erase(p->second);
    events.erase(p);
  }
}

void SafeTimer::dump(const char *caller) const
{
  if (!caller)
    caller = "";
  dout(10) << "dump " << caller << dendl;

  for (scheduled_map_t::const_iterator s = schedule.begin();
       s != schedule.end();
       ++s)
    dout(10) << " " << s->first << "->" << s->second << dendl;
}
