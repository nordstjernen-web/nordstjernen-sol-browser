/* Nordstjernen — civil-date math and HTML date/time string parsing.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */
#ifndef NS_DATETIME_H
#define NS_DATETIME_H

#include <glib.h>

#define NS_DT_MAX_YEAR 275760

long ns_dt_floormod(long a, long b);
long ns_dt_days_from_civil(int y, int m, int d);
void ns_dt_civil_from_days(long z, int *y, int *m, int *d);
int  ns_dt_iso_weeks_in_year(int y);
long ns_dt_iso_week1_monday(int y);
const char *ns_dt_rd_digits(const char *p, int min, int max, int *out);
const char *ns_dt_rd_date(const char *p, int *y, int *m, int *d);
const char *ns_dt_rd_time(const char *p, int *ms);

#endif
