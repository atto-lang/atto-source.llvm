#!/usr/bin/env python3
"""Source-only fail-closed contract for the LLVM private CI V3 publisher.

This audit cannot enable export, authenticate GitHub's live rulesets or
certify an exclusive remote ref namespace. It prevents accidental removal
of the current hard DENY and reintroduction of unsafe name-only deletion.
"""
import ast
from pathlib import Path
import stat
import sys


class CustodyContractError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise CustodyContractError(message)


def function(tree, name):
    matches = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name == name
    ]
    require(len(matches) == 1, "missing or duplicated " + name)
    require(isinstance(matches[0], ast.FunctionDef),
            "publisher API became async: " + name)
    return matches[0]


def direct_call(stmt, name):
    return (isinstance(stmt, ast.Expr)
            and isinstance(stmt.value, ast.Call)
            and isinstance(stmt.value.func, ast.Name)
            and stmt.value.func.id == name
            and not stmt.value.args
            and not stmt.value.keywords)


def call_name(node):
    return node.func.id if isinstance(node, ast.Call) and isinstance(
        node.func, ast.Name
    ) else None


def is_deny_raise(node):
    return (isinstance(node, ast.Raise)
            and isinstance(node.exc, ast.Call)
            and isinstance(node.exc.func, ast.Name)
            and node.exc.func.id == "Denied"
            and node.exc.args
            and isinstance(node.exc.args[0], ast.Constant)
            and isinstance(node.exc.args[0].value, str))


def source_literals(node):
    # ast.JoinedStr preserves f-string's static parts as Constant nodes.
    return {
        child.value for child in ast.walk(node)
        if isinstance(child, ast.Constant)
        and isinstance(child.value, str)
    }


def audit_module_root(tree):
    """Reject dynamic module-scope overrides of the publisher deny gate.

    The real publisher intentionally declares only imports, uppercase
    constants, one Denied exception class, functions, and its exact CLI
    entry point. No runtime module-level exec, assignment to a function,
    alias import, decorator, or conditional rebinding is permitted.
    """
    main_guards = 0
    exception_classes = 0
    for index, statement in enumerate(tree.body):
        if (index == 0 and isinstance(statement, ast.Expr)
                and isinstance(statement.value, ast.Constant)
                and isinstance(statement.value.value, str)):
            continue
        if isinstance(statement, (ast.Import, ast.ImportFrom)):
            names = [
                alias.asname or alias.name.split(".", 1)[0]
                for alias in statement.names
            ]
            require(all(name not in {
                "require_atomic_ref_rollback", "publish", "Denied",
                "delete_exact_ref", "_gh_ref_request",
                "delete_handoff_refs_atomic"
            } for name in names), "protected function shadowed by import")
            continue
        if isinstance(statement, ast.Assign):
            require(len(statement.targets) == 1
                    and isinstance(statement.targets[0], ast.Name)
                    and statement.targets[0].id.isupper(),
                    "module assignment may only declare uppercase constants")
            continue
        if isinstance(statement, ast.ClassDef):
            exception_classes += 1
            require(exception_classes == 1
                    and statement.name == "Denied"
                    and not statement.decorator_list
                    and len(statement.bases) == 1
                    and isinstance(statement.bases[0], ast.Name)
                    and statement.bases[0].id == "RuntimeError",
                    "publisher must have one undecorated Denied exception class")
            continue
        if isinstance(statement, ast.FunctionDef):
            require(not statement.decorator_list,
                    "publisher functions must not have decorators")
            continue
        require(isinstance(statement, ast.If),
                "unexpected module-scope statement")
        main_guards += 1
        require(main_guards == 1 and index == len(tree.body) - 1
                and not statement.orelse,
                "CLI entry must be unique and last")
        test = statement.test
        require(isinstance(test, ast.Compare)
                and isinstance(test.left, ast.Name)
                and test.left.id == "__name__"
                and len(test.ops) == 1
                and isinstance(test.ops[0], ast.Eq)
                and len(test.comparators) == 1
                and isinstance(test.comparators[0], ast.Constant)
                and test.comparators[0].value == "__main__",
                "module conditional must be exact CLI entry")
        require(len(statement.body) == 1
                and isinstance(statement.body[0], ast.Expr),
                "CLI entry must have exactly one expression")
        call = statement.body[0].value
        require(isinstance(call, ast.Call)
                and isinstance(call.func, ast.Attribute)
                and isinstance(call.func.value, ast.Name)
                and call.func.value.id == "sys"
                and call.func.attr == "exit"
                and len(call.args) == 1 and not call.keywords
                and isinstance(call.args[0], ast.Call)
                and isinstance(call.args[0].func, ast.Name)
                and call.args[0].func.id == "main"
                and not call.args[0].args
                and not call.args[0].keywords,
                "CLI entry must invoke only sys.exit(main())")
    require(main_guards == 1, "publisher CLI entry missing")
    require(exception_classes == 1, "publisher Denied exception class missing")


def audit_source(source):
    require(isinstance(source, str), "source must be UTF-8 text")
    require(len(source.encode("utf-8")) <= 512 * 1024,
            "publisher exceeds bounded source size")
    try:
        tree = ast.parse(source)
    except (SyntaxError, ValueError) as exc:
        raise CustodyContractError("publisher syntax is invalid") from exc
    audit_module_root(tree)

    guard = function(tree, "require_atomic_ref_rollback")
    guard_body = guard.body
    if guard_body and isinstance(guard_body[0], ast.Expr):
        expr = guard_body[0].value
        if isinstance(expr, ast.Constant) and isinstance(expr.value, str):
            guard_body = guard_body[1:]
    require(len(guard_body) == 1 and is_deny_raise(guard_body[0]),
            "production export guard must unconditionally raise Denied")
    require("private CI export disabled" in guard_body[0].exc.args[0].value,
            "production export guard must state disabled status")

    publish = function(tree, "publish")
    indices = [
        i for i, statement in enumerate(publish.body)
        if direct_call(statement, "require_atomic_ref_rollback")
    ]
    require(len(indices) == 1,
            "publish must call the guard exactly once at top level")
    guard_index = indices[0]
    forbidden_before = {
        "protected_authority", "exact_source_tree", "gh_api",
        "run_bounded_gh", "checked_destination", "verify_source_pr",
    }
    for statement in publish.body[:guard_index]:
        require(not any(
            isinstance(node, (ast.Return, ast.Yield, ast.YieldFrom, ast.Await))
            for node in ast.walk(statement)
        ), "publisher may not return or suspend before export guard")
        for node in ast.walk(statement):
            if not isinstance(node, ast.Call):
                continue
            require(call_name(node) not in forbidden_before,
                    "publisher accessed private authority before export guard")
            if isinstance(node.func, ast.Name):
                require(node.func.id in {"isinstance", "valid_sha"},
                        "unexpected direct call before export guard")
            elif isinstance(node.func, ast.Attribute):
                require(node.func.attr in {
                    "fullmatch", "utcoffset", "now", "astimezone", "replace"
                }, "unexpected attribute call before export guard")
            else:
                raise CustodyContractError(
                    "dynamic call before export guard is forbidden")

    ref_request = function(tree, "_gh_ref_request")
    early_rest_deny = False
    for statement in ref_request.body:
        if (isinstance(statement, ast.If)
                and isinstance(statement.test, ast.Compare)
                and isinstance(statement.test.left, ast.Name)
                and statement.test.left.id == "method"
                and len(statement.test.ops) == 1
                and isinstance(statement.test.ops[0], ast.NotEq)
                and len(statement.test.comparators) == 1
                and isinstance(statement.test.comparators[0], ast.Constant)
                and statement.test.comparators[0].value == "GET"
                and statement.body
                and is_deny_raise(statement.body[0])):
            early_rest_deny = True
            break
    require(early_rest_deny,
            "ref REST helper must fail closed for every method except GET")

    rest_delete = function(tree, "delete_exact_ref")
    require(bool(rest_delete.body) and is_deny_raise(rest_delete.body[-1]),
            "name-only ref cleanup must terminate in unconditional denial")
    require(not any(
        call_name(node) in {"run_bounded_gh", "_gh_ref_request"}
        for node in ast.walk(rest_delete)
        if isinstance(node, ast.Call)
    ), "name-only delete gained a Git/REST transport")

    atomic = function(tree, "delete_handoff_refs_atomic")
    literal_parts = source_literals(atomic)
    require("--atomic" in literal_parts,
            "generation cleanup lost atomic multi-ref push")
    require("--force-with-lease=refs/heads/" in literal_parts,
            "generation cleanup lost old-SHA lease")
    require(not any(
        call_name(node) == "_gh_ref_request"
        for node in ast.walk(atomic)
        if isinstance(node, ast.Call)
    ), "atomic cleanup acquired REST name-only ref access")
    return True


def main():
    path = Path(__file__).with_name("publish-private-ci-source-v3.py")
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode), "publisher is not a regular file")
    require(info.st_size <= 512 * 1024, "publisher file too large")
    audit_source(path.read_text(encoding="utf-8"))
    print("PRIVATE_CI_REF_CUSTODY_FAIL_CLOSED_AUDIT=PASS")


if __name__ == "__main__":
    try:
        main()
    except (CustodyContractError, OSError, UnicodeError) as exc:
        print("PRIVATE_CI_REF_CUSTODY_FAIL_CLOSED_AUDIT=DENY: " + str(exc),
              file=sys.stderr)
        sys.exit(1)
