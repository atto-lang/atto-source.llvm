// Generated from stdlib/std/path.atto; do not edit manually.
// source Git blob SHA-1: adde8472c218d9a5a6d0644fd9ccefc80cf3d8f6
R"ATTO_STD_PATH(// LLVM-owned native Atto std/path lexical scanner policies.
// These are building blocks for an eventual no-host-interop Path/PathBuf.
// A path byte scanner must enforce NUL/control, separator and component rules;
// the trusted FS provider MUST still resolve symlinks, roots and capabilities.
define module path as
  pub fnc is_separator(int scalar, bool windows): bool {
    return scalar == 47 || (windows && scalar == 92);
  }

  pub fnc is_ascii_letter(int scalar): bool {
    return (scalar >= 65 && scalar <= 90) ||
           (scalar >= 97 && scalar <= 122);
  }

  pub fnc is_safe_scalar(int scalar, bool windows): bool {
    if scalar < 32 || scalar == 127 || scalar > 1114111 then return false; fi
    if scalar >= 55296 && scalar <= 57343 then return false; fi
    if is_separator(scalar, windows) then return false; fi
    if windows; do
      return is_ascii_letter(scalar) ||
             (scalar >= 48 && scalar <= 57) ||
             scalar == 46 || scalar == 45 || scalar == 95;
    fi
    return true;
  }

  // Lexical component classifier for a scanner that has already checked
  // every codepoint: 0 ordinary, 1 dot, 2 dotdot, -1 invalid.
  pub fnc component_kind(int length, int first, int second): int {
    if length < 1 || length > 255 then return -1; fi
    if length == 1 && first == 46 then return 1; fi
    if length == 2 && first == 46 && second == 46 then return 2; fi
    return 0;
  }

  // Component depth limit is 256. Checked subtraction prevents escaping root.
  // The scanner must reject absolute/drive/UNC paths before calling this.
  pub fnc advance_relative_depth(int depth, int component): int {
    if depth < 0 || depth > 256 then return -1; fi
    if component == 1 then return depth; fi
    if component == 2; do
      if depth == 0 then return -1; fi
      return depth - 1;
    fi
    if component != 0 || depth == 256 then return -1; fi
    return depth + 1;
  }

  pub fnc is_windows_drive_prefix(int first, int second): bool {
    return is_ascii_letter(first) && second == 58;
  }

  // A successful lexical budget check is not native filesystem authority.
  // Inputs are already checked per-component by the caller's source scanner.
  pub fnc relative_join_budget_admissible(int left_scalars, int right_scalars,
                                           int left_components, int right_components): bool {
    if !scanner_budget_admissible(left_scalars, left_components) then return false; fi
    if !scanner_budget_admissible(right_scalars, right_components) then return false; fi
    // Reserve one separator without addition that could wrap.
    if left_scalars > 4095 - right_scalars then return false; fi
    return left_components <= 256 - right_components;
  }

  // Only dots with a nonempty basename and suffix can start an extension.
  // Dotfile ".env", leading dot and trailing dot are not extensions.
  pub fnc extension_offset_admissible(int filename_scalars, int dot_index): bool {
    if filename_scalars < 3 || filename_scalars > 255 then return false; fi
    return dot_index >= 1 && dot_index < filename_scalars - 1;
  }

  fnc to_upper_ascii(int cp): int {
    if cp >= 97 && cp <= 122 then return cp - 32; fi
    return cp;
  }

  // All recognized Windows aliases remain reserved when an extension follows.
  // The full scanner must validate the complete stem and suffix separately.
  pub fnc windows_device_3(int first, int second, int third): bool {
    let int a = to_upper_ascii(first);
    let int b = to_upper_ascii(second);
    let int c = to_upper_ascii(third);
    if a == 67 && b == 79 && c == 78 then return true; fi
    if a == 80 && b == 82 && c == 78 then return true; fi
    if a == 65 && b == 85 && c == 88 then return true; fi
    return a == 78 && b == 85 && c == 76;
  }

  pub fnc windows_device_4(int first, int second, int third, int fourth): bool {
    if fourth < 49 || fourth > 57 then return false; fi
    let int a = to_upper_ascii(first);
    let int b = to_upper_ascii(second);
    let int c = to_upper_ascii(third);
    return (a == 67 && b == 79 && c == 77) ||
           (a == 76 && b == 80 && c == 84);
  }

  // Root/handle confinement is NOT a consequence of this lexical predicate.
  pub fnc scanner_budget_admissible(int scalars, int components): bool {
    if scalars < 1 || scalars > 4096 then return false; fi
    return components >= 0 && components <= 256;
  }
end
)ATTO_STD_PATH"
