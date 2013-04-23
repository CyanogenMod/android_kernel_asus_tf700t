/*
 * include/trace/events/nvevent.h
 *
 * Input event logging to ftrace.
 *
 * Copyright (c) 2012, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM nvevent

#if !defined(_TRACE_NVEVENT_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_NVEVENT_H

#include <linux/ktime.h>
#include <linux/tracepoint.h>

TRACE_EVENT(nvevent_irq_data_read_start_series,
	TP_PROTO(const char *name),
	TP_ARGS(name),
	TP_STRUCT__entry(
		__field(const char *, name)
	),
	TP_fast_assign(
		__entry->name = name;
	),
	TP_printk("name=%s",
	  __entry->name)
);

TRACE_EVENT(nvevent_irq_data_read_finish_series,
	TP_PROTO(const char *name),
	TP_ARGS(name),
	TP_STRUCT__entry(
		__field(const char *, name)
	),
	TP_fast_assign(
		__entry->name = name;
	),
	TP_printk("name=%s",
	  __entry->name)
);

TRACE_EVENT(nvevent_irq_data_read_start_single,
	TP_PROTO(const char *name),
	TP_ARGS(name),
	TP_STRUCT__entry(
		__field(const char *, name)
	),
	TP_fast_assign(
		__entry->name = name;
	),
	TP_printk("name=%s",
	  __entry->name)
);

TRACE_EVENT(nvevent_irq_data_read_finish_single,
	TP_PROTO(const char *name),
	TP_ARGS(name),
	TP_STRUCT__entry(
		__field(const char *, name)
	),
	TP_fast_assign(
		__entry->name = name;
	),
	TP_printk("name=%s",
	  __entry->name)
);

TRACE_EVENT(nvevent_irq_data_submit,
	TP_PROTO(const char *name),
	TP_ARGS(name),
	TP_STRUCT__entry(
		__field(const char *, name)
	),
	TP_fast_assign(
		__entry->name = name;
	),
	TP_printk("name=%s",
	  __entry->name)
);

#endif /*  _TRACE_NVEVENT_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
