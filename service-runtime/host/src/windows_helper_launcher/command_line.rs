use std::ffi::OsString;
use std::os::windows::ffi::OsStrExt;
use std::path::Path;

pub(super) fn command_line(executable: &Path, arguments: &[OsString]) -> Vec<u16> {
    let mut result = Vec::new();
    append_quoted(&mut result, executable.as_os_str());
    for argument in arguments {
        result.push(b' ' as u16);
        append_quoted(&mut result, argument);
    }
    result.push(0);
    result
}

fn append_quoted(output: &mut Vec<u16>, argument: &std::ffi::OsStr) {
    output.push(b'"' as u16);
    let mut slashes = 0usize;
    for value in argument.encode_wide() {
        if value == b'\\' as u16 {
            slashes += 1;
        } else if value == b'"' as u16 {
            output.extend(std::iter::repeat(b'\\' as u16).take(slashes * 2 + 1));
            output.push(value);
            slashes = 0;
        } else {
            output.extend(std::iter::repeat(b'\\' as u16).take(slashes));
            output.push(value);
            slashes = 0;
        }
    }
    output.extend(std::iter::repeat(b'\\' as u16).take(slashes * 2));
    output.push(b'"' as u16);
}
