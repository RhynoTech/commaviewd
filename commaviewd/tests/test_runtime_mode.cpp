#include "mode.h"

#include <cassert>
#include <string>

using commaview::runtime::ParsedMode;
using commaview::runtime::RuntimeMode;

static void test_explicit_bridge_mode() {
  char arg0[] = "commaviewd";
  char arg1[] = "bridge";
  char* argv[] = {arg0, arg1};

  ParsedMode parsed = commaview::runtime::parse_mode(2, argv);
  assert(parsed.ok);
  assert(parsed.mode == RuntimeMode::kBridge);
  assert(parsed.mode_arg_index == 1);
}

static void test_explicit_control_mode() {
  char arg0[] = "commaviewd";
  char arg1[] = "control";
  char* argv[] = {arg0, arg1};

  ParsedMode parsed = commaview::runtime::parse_mode(2, argv);
  assert(parsed.ok);
  assert(parsed.mode == RuntimeMode::kControl);
  assert(parsed.mode_arg_index == 1);
}

static void test_missing_mode_fails_even_with_legacy_name() {
  char arg0[] = "commaview-bridge";
  char* argv[] = {arg0};

  ParsedMode parsed = commaview::runtime::parse_mode(1, argv);
  assert(!parsed.ok);
  assert(parsed.error == "missing mode argument");
}

static void test_unknown_mode_fails() {
  char arg0[] = "commaviewd";
  char arg1[] = "banana";
  char* argv[] = {arg0, arg1};

  ParsedMode parsed = commaview::runtime::parse_mode(2, argv);
  assert(!parsed.ok);
  assert(parsed.error.find("unknown mode") != std::string::npos);
}

int main() {
  test_explicit_bridge_mode();
  test_explicit_control_mode();
  test_missing_mode_fails_even_with_legacy_name();
  test_unknown_mode_fails();
  return 0;
}
