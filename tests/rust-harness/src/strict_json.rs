use std::collections::BTreeMap;

#[derive(Debug, Clone, PartialEq)]
enum Value {
    Null,
    Bool(bool),
    Number(String),
    String(String),
    Array,
    Object(BTreeMap<String, Value>),
}

struct Parser<'a> {
    bytes: &'a [u8],
    pos: usize,
    depth: usize,
}

impl<'a> Parser<'a> {
    fn new(text: &'a str) -> Self {
        Self { bytes: text.as_bytes(), pos: 0, depth: 0 }
    }

    fn parse(mut self) -> Result<Value, String> {
        self.ws();
        let value = self.value()?;
        self.ws();
        if self.pos != self.bytes.len() {
            return Err("trailing JSON data".into());
        }
        Ok(value)
    }

    fn ws(&mut self) {
        while self.pos < self.bytes.len()
            && matches!(self.bytes[self.pos], b' ' | b'\n' | b'\r' | b'\t')
        {
            self.pos += 1;
        }
    }

    fn value(&mut self) -> Result<Value, String> {
        if self.depth >= 64 {
            return Err("JSON nesting limit exceeded".into());
        }
        self.depth += 1;
        let result = match self.bytes.get(self.pos).copied() {
            Some(b'n') => { self.literal(b"null")?; Ok(Value::Null) }
            Some(b't') => { self.literal(b"true")?; Ok(Value::Bool(true)) }
            Some(b'f') => { self.literal(b"false")?; Ok(Value::Bool(false)) }
            Some(b'"') => self.string().map(Value::String),
            Some(b'[') => self.array(),
            Some(b'{') => self.object(),
            Some(b'-' | b'0'..=b'9') => self.number().map(Value::Number),
            _ => Err("invalid JSON value".into()),
        };
        self.depth -= 1;
        result
    }

    fn literal(&mut self, expected: &[u8]) -> Result<(), String> {
        if self.bytes.get(self.pos..self.pos + expected.len()) == Some(expected) {
            self.pos += expected.len();
            Ok(())
        } else {
            Err("invalid JSON literal".into())
        }
    }

    fn array(&mut self) -> Result<Value, String> {
        self.pos += 1;
        self.ws();
        if self.bytes.get(self.pos) == Some(&b']') {
            self.pos += 1;
            return Ok(Value::Array);
        }
        loop {
            self.ws();
            self.value()?;
            self.ws();
            match self.bytes.get(self.pos).copied() {
                Some(b',') => { self.pos += 1; }
                Some(b']') => { self.pos += 1; break; }
                _ => return Err("invalid JSON array".into()),
            }
        }
        Ok(Value::Array)
    }

    fn object(&mut self) -> Result<Value, String> {
        self.pos += 1;
        self.ws();
        let mut values = BTreeMap::new();
        if self.bytes.get(self.pos) == Some(&b'}') {
            self.pos += 1;
            return Ok(Value::Object(values));
        }
        loop {
            self.ws();
            let key = self.string()?;
            self.ws();
            if self.bytes.get(self.pos) != Some(&b':') {
                return Err("missing JSON object colon".into());
            }
            self.pos += 1;
            self.ws();
            let value = self.value()?;
            if values.insert(key, value).is_some() {
                return Err("duplicate JSON object key".into());
            }
            self.ws();
            match self.bytes.get(self.pos).copied() {
                Some(b',') => { self.pos += 1; }
                Some(b'}') => { self.pos += 1; break; }
                _ => return Err("invalid JSON object".into()),
            }
        }
        Ok(Value::Object(values))
    }

    fn number(&mut self) -> Result<String, String> {
        let start = self.pos;
        if self.bytes.get(self.pos) == Some(&b'-') {
            self.pos += 1;
        }
        match self.bytes.get(self.pos).copied() {
            Some(b'0') => {
                self.pos += 1;
                if matches!(self.bytes.get(self.pos).copied(), Some(b'0'..=b'9')) {
                    return Err("leading zero in JSON number".into());
                }
            }
            Some(b'1'..=b'9') => {
                self.pos += 1;
                while matches!(self.bytes.get(self.pos).copied(), Some(b'0'..=b'9')) {
                    self.pos += 1;
                }
            }
            _ => return Err("invalid JSON number".into()),
        }
        if self.bytes.get(self.pos) == Some(&b'.') {
            self.pos += 1;
            let first = self.pos;
            while matches!(self.bytes.get(self.pos).copied(), Some(b'0'..=b'9')) {
                self.pos += 1;
            }
            if self.pos == first {
                return Err("invalid JSON fraction".into());
            }
        }
        if matches!(self.bytes.get(self.pos).copied(), Some(b'e' | b'E')) {
            self.pos += 1;
            if matches!(self.bytes.get(self.pos).copied(), Some(b'+' | b'-')) {
                self.pos += 1;
            }
            let first = self.pos;
            while matches!(self.bytes.get(self.pos).copied(), Some(b'0'..=b'9')) {
                self.pos += 1;
            }
            if self.pos == first {
                return Err("invalid JSON exponent".into());
            }
        }
        std::str::from_utf8(&self.bytes[start..self.pos])
            .map(str::to_owned)
            .map_err(|_| "invalid UTF-8 in JSON number".into())
    }

    fn string(&mut self) -> Result<String, String> {
        if self.bytes.get(self.pos) != Some(&b'"') {
            return Err("expected JSON string".into());
        }
        self.pos += 1;
        let mut out = String::new();
        let mut segment = self.pos;
        while self.pos < self.bytes.len() {
            match self.bytes[self.pos] {
                b'"' => {
                    self.push_segment(&mut out, segment, self.pos)?;
                    self.pos += 1;
                    return Ok(out);
                }
                b'\\' => {
                    self.push_segment(&mut out, segment, self.pos)?;
                    self.pos += 1;
                    let esc = *self.bytes.get(self.pos).ok_or("truncated JSON escape")?;
                    self.pos += 1;
                    match esc {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\u{0008}'),
                        b'f' => out.push('\u{000c}'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let first = self.hex4()?;
                            if (0xD800..=0xDBFF).contains(&first) {
                                if self.bytes.get(self.pos..self.pos + 2) != Some(&b"\\u"[..]) {
                                    return Err("missing low surrogate".into());
                                }
                                self.pos += 2;
                                let second = self.hex4()?;
                                if !(0xDC00..=0xDFFF).contains(&second) {
                                    return Err("invalid low surrogate".into());
                                }
                                let cp = 0x10000
                                    + (((first as u32 - 0xD800) << 10)
                                    | (second as u32 - 0xDC00));
                                out.push(char::from_u32(cp).ok_or("invalid Unicode code point")?);
                            } else if (0xDC00..=0xDFFF).contains(&first) {
                                return Err("unpaired low surrogate".into());
                            } else {
                                out.push(char::from_u32(first as u32).ok_or("invalid Unicode code point")?);
                            }
                        }
                        _ => return Err("invalid JSON escape".into()),
                    }
                    segment = self.pos;
                }
                byte if byte < 0x20 => return Err("control character in JSON string".into()),
                _ => self.pos += 1,
            }
        }
        Err("unterminated JSON string".into())
    }

    fn push_segment(&self, out: &mut String, start: usize, end: usize) -> Result<(), String> {
        let segment = std::str::from_utf8(&self.bytes[start..end])
            .map_err(|_| "invalid UTF-8 in JSON string")?;
        out.push_str(segment);
        Ok(())
    }

    fn hex4(&mut self) -> Result<u16, String> {
        let bytes = self.bytes.get(self.pos..self.pos + 4).ok_or("truncated Unicode escape")?;
        let mut value = 0u16;
        for &byte in bytes {
            value = value
                .checked_mul(16)
                .ok_or("Unicode escape overflow")?
                + match byte {
                    b'0'..=b'9' => (byte - b'0') as u16,
                    b'a'..=b'f' => (byte - b'a' + 10) as u16,
                    b'A'..=b'F' => (byte - b'A' + 10) as u16,
                    _ => return Err("invalid Unicode escape".into()),
                };
        }
        self.pos += 4;
        Ok(value)
    }
}

fn object(text: &str) -> Option<BTreeMap<String, Value>> {
    match Parser::new(text).parse().ok()? {
        Value::Object(values) => Some(values),
        _ => None,
    }
}

pub fn bool_field(text: &str, key: &str) -> Option<bool> {
    match object(text)?.get(key)? {
        Value::Bool(value) => Some(*value),
        _ => None,
    }
}

pub fn u64_field(text: &str, key: &str) -> Option<u64> {
    match object(text)?.get(key)? {
        Value::Number(value)
            if !value.starts_with('-')
                && !value.contains('.')
                && !value.contains('e')
                && !value.contains('E') => value.parse().ok(),
        _ => None,
    }
}

pub fn string_field(text: &str, key: &str) -> Option<String> {
    match object(text)?.get(key)? {
        Value::String(value) => Some(value.clone()),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_prefix_and_trailing_garbage() {
        assert_eq!(bool_field(r#"{"ok":truegarbage}"#, "ok"), None);
        assert_eq!(u64_field(r#"{"n":2oops}"#, "n"), None);
        assert_eq!(string_field(r#"{"s":"ok"} trailing"#, "s"), None);
    }

    #[test]
    fn does_not_match_nested_or_string_keys() {
        assert_eq!(bool_field(r#"{"nested":{"ok":true}}"#, "ok"), None);
        assert_eq!(bool_field(r#"{"text":"\"ok\": true","ok":false}"#, "ok"), Some(false));
    }

    #[test]
    fn rejects_duplicate_keys_and_wrong_types() {
        assert_eq!(bool_field(r#"{"ok":true,"ok":false}"#, "ok"), None);
        assert_eq!(bool_field(r#"{"ok":"true"}"#, "ok"), None);
        assert_eq!(u64_field(r#"{"n":-1}"#, "n"), None);
        assert_eq!(u64_field(r#"{"n":2.0}"#, "n"), None);
    }

    #[test]
    fn decodes_json_strings() {
        assert_eq!(
            string_field(r#"{"s":"RT008_\u0055NBOUNDED_LOOP"}"#, "s").as_deref(),
            Some("RT008_UNBOUNDED_LOOP")
        );
    }
}
