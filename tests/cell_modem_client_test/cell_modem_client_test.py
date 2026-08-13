def test_no_device_failures(emulated_output_lines):
    begin_i = emulated_output_lines.index("BEGIN-TEST")
    end_i = emulated_output_lines.index("END-TEST")
    test_lines = emulated_output_lines[begin_i : end_i + 1]
    assert test_lines
