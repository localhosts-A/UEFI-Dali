// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pure bounded policy parser, also compiled by the native host tests.
#ifndef DALI_BOOT_POLICY_H
#define DALI_BOOT_POLICY_H
typedef struct {
  unsigned AutoFastboot, TimeoutSeconds, Qualification, Method;
  char Token[33];
} DALI_BOOT_POLICY;

static int daliPolicyEqual(const char *a, const char *b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}
static int daliPolicyLine(const char **at, const char *end, char line[96]) {
  unsigned n = 0;
  while (*at < end && **at != '\n') {
    if (n == 95 || **at == 0) { return 0; }
    line[n++] = *(*at)++;
  }
  if (*at == end) { return 0; }
  ++*at;
  line[n] = 0;
  return 1;
}
static const char *daliPolicyValue(const char *line, const char *prefix) {
  while (*prefix && *line == *prefix) { ++line; ++prefix; }
  return *prefix ? (const char *)0 : line;
}
static const char *daliPolicyMethodName(unsigned method) {
  switch (method) {
    case 0: return "off";
    case 1: return "psci-cold";
    case 2: return "psci-warm";
    case 3: return "aee";
    default: return "invalid";
  }
}
static int daliParseBootPolicy(const char *data, unsigned size, DALI_BOOT_POLICY *out) {
  const char *at = data, *end = data + size, *value;
  char line[96];
  DALI_BOOT_POLICY p = {0, 0, 0, 0, {0}};
  if (size == 0 || size >= 256 || !daliPolicyLine(&at, end, line)) { return 0; }
  if (daliPolicyEqual(line, "version = 2")) { p.Qualification = 1; }
  else if (!daliPolicyEqual(line, "version = 1")) { return 0; }
  if (!daliPolicyLine(&at, end, line)) { return 0; }
  if (daliPolicyEqual(line, "auto_fastboot = true")) { p.AutoFastboot = 1; }
  else if (!daliPolicyEqual(line, "auto_fastboot = false")) { return 0; }
  if (!daliPolicyLine(&at, end, line) ||
      !(value = daliPolicyValue(line, "watchdog_timeout_seconds = ")) || !*value) { return 0; }
  for (; *value; ++value) {
    if (*value < '0' || *value > '9' || p.TimeoutSeconds > 31) { return 0; }
    p.TimeoutSeconds = p.TimeoutSeconds * 10 + (unsigned)(*value - '0');
  }
  if (p.TimeoutSeconds < 2 || p.TimeoutSeconds > 31) { return 0; }
  if (!daliPolicyLine(&at, end, line) || !(value = daliPolicyValue(line, "run_token = "))) { return 0; }
  if (daliPolicyEqual(value, "none") && !p.Qualification) {
    p.Token[0] = 'n'; p.Token[1] = 'o'; p.Token[2] = 'n'; p.Token[3] = 'e';
  } else {
    for (unsigned i = 0; i < 32; ++i) {
      char c = value[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { return 0; }
      p.Token[i] = c;
    }
    if (value[32]) { return 0; }
  }
  if (p.Qualification) {
    if (p.AutoFastboot || !daliPolicyLine(&at, end, line) ||
        !(value = daliPolicyValue(line, "reset_method = "))) { return 0; }
    for (; p.Method < 4; ++p.Method) {
      if (daliPolicyEqual(value, daliPolicyMethodName(p.Method))) { break; }
    }
    if (p.Method == 4 || !daliPolicyLine(&at, end, line) ||
        !daliPolicyEqual(line, "reset_delay_seconds = 5")) { return 0; }
  }
  if (at != end) { return 0; }
  *out = p;
  return 1;
}
#endif
