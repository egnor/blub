import re

FIRMWARE_DIR = "line_parser_firmware"


def test_no_device_failures(emulated_output, device_checks):
    fails = re.findall(r"^FAIL .*$", emulated_output, re.M)
    assert not fails, "device-side check(s) failed:\n" + "\n".join(fails)
    assert device_checks["failures"] == 0


def test_all_checks_ran(device_checks):
    # Guards against the firmware bailing out early and still printing END-TEST.
    assert device_checks["checks"] == 26


def test_running_on_32_bit_target(device_info):
    # The point of emulating rather than compiling for the host: this is a
    # 32-bit target, and code under test gets to prove it behaves like one.
    assert device_info["sizeof_pointer"] == "4"
    assert device_info["sizeof_size_t"] == "4"


def test_parser_stays_small(device_info):
    # Host-side matchers can express things device-side asserts shouldn't
    # hardcode -- here, a size budget that's easy to review and adjust.
    assert int(device_info["sizeof_LineParser"]) <= 24


def test_truncation_reported_the_line_it_kept(emulated_output):
    assert re.search(r"^INFO truncated_to=0123456$", emulated_output, re.M)
