//! Independent *host-only* reference for the small Atto-authored
//! three-byte source-to-Wasm instruction fragment kernel.
//! These tests do NOT compile or execute the .atto file.

fn digit(ascii: i64) -> Option<i64> {
    (48..=57).contains(&ascii).then_some(ascii - 48)
}

fn compile(lhs: i64, op: i64, rhs: i64) -> Result<i64, &'static str> {
    let left = digit(lhs).ok_or("bad lhs")?;
    let right = digit(rhs).ok_or("bad rhs")?;
    let value = match op {
        43 => left.checked_add(right),
        45 => left.checked_sub(right),
        42 => left.checked_mul(right),
        _ => return Err("bad op"),
    }
    .ok_or("overflow")?;
    if !(-64..=63).contains(&value) {
        return Err("one-byte sleb128 range");
    }
    let immediate = if value < 0 { value + 128 } else { value };
    Ok(0x42 + (immediate << 8) + (0x0b << 16))
}

fn decode_fragment(fragment: i64) -> i64 {
    assert!(fragment >= 0);
    assert_eq!((fragment & 0xff) as u8, 0x42, "i64.const");
    assert_eq!(((fragment >> 16) & 0xff) as u8, 0x0b, "end");
    assert_eq!(fragment >> 24, 0, "exact three bytes");
    let imm = (fragment >> 8) & 0xff;
    assert_eq!(imm & 0x80, 0, "one-byte signed LEB128");
    if imm & 0x40 != 0 { imm - 128 } else { imm }
}

#[test]
fn actual_binary_encoding_vectors() {
    let vectors = [
        (b'6', b'*', b'7', 731714_i64, 42_i64),
        (b'9', b'+', b'2', 723778, 11),
        (b'3', b'-', b'9', 752194, -6),
        (b'0', b'+', b'0', 720962, 0),
        (b'7', b'*', b'9', 737090, 63),
    ];
    for (lhs, op, rhs, expected_bytes, expected_result) in vectors {
        let actual = compile(lhs.into(), op.into(), rhs.into()).unwrap();
        assert_eq!(actual, expected_bytes);
        assert_eq!(decode_fragment(actual), expected_result);
    }
}

#[test]
fn negative_input_admission() {
    assert_eq!(compile(b'x'.into(), b'+'.into(), b'2'.into()), Err("bad lhs"));
    assert_eq!(compile(b'1'.into(), b'+'.into(), b'!'.into()), Err("bad rhs"));
    assert_eq!(compile(b'1'.into(), b'/'.into(), b'2'.into()), Err("bad op"));
    assert_eq!(compile(b'9'.into(), b'*'.into(), b'9'.into()), Err("one-byte sleb128 range"));
    assert_eq!(compile(b'8'.into(), b'*'.into(), b'8'.into()), Err("one-byte sleb128 range"));
}

#[test]
fn distinct_inputs_create_distinct_fragments() {
    let a = compile(b'6'.into(), b'*'.into(), b'7'.into()).unwrap();
    let b = compile(b'9'.into(), b'+'.into(), b'2'.into()).unwrap();
    assert_ne!(a, b);
    assert_eq!(a - 731672, 42);
}
