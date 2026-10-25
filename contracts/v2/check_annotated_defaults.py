#!/usr/bin/env python3
"""Reference-only validator for annotated default parameter rules.

This does NOT parse or execute Oreslang. It checks the independently encoded
contract, including source evaluation provenance and exact option lifting.
"""
from dataclasses import dataclass
from typing import Optional
import random

class Reject(ValueError):
    pass

@dataclass(frozen=True)
class Formal:
    name: str
    typ: str
    default: Optional[tuple] = None  # (kind, value); explicit ('none', None)
    required: bool = False

@dataclass(frozen=True)
class Actual:
    value: str = ''
    typ: str = ''
    name: Optional[str] = None
    omit: bool = False

def underlying(typ):
    if typ.startswith('Option<') and typ.endswith('>'):
        return typ[7:-1]
    return None

def checked(formals, v2=True):
    names = set()
    for f in formals:
        if f.name in names:
            raise Reject('duplicate formal name')
        names.add(f.name)
        t = underlying(f.typ)
        if f.required and t is None:
            raise Reject('required must apply to an Option')
        if f.required and f.default is not None:
            raise Reject('required Option cannot have a default')
        if f.default is not None:
            kind, value = f.default
            if kind == 'none':
                if t is None or value is not None:
                    raise Reject('None default requires Option')
            else:
                if t is not None and underlying(t) is not None:
                    raise Reject('nested Option default requires explicit typed outer Option')
                expected = t if t is not None else f.typ
                if kind != expected:
                    raise Reject('default literal type mismatch')
    return formals

def optional(f, v2=True):
    return f.default is not None or (v2 and underlying(f.typ) is not None and not f.required)

def bind(formals, actuals, v2=True):
    checked(formals, v2)
    lookup = {f.name: i for i, f in enumerate(formals)}
    slots = [None] * len(formals)
    presence = ['absent'] * len(formals)
    evaluated_items = []
    positional = 0
    seen_named = False
    for item_index, a in enumerate(actuals):
        if a.name is not None:
            seen_named = True
            if a.name not in lookup:
                raise Reject('unknown named argument')
            slot = lookup[a.name]
        else:
            if seen_named:
                raise Reject('positional after named')
            if positional >= len(formals):
                raise Reject('too many positional args')
            slot = positional
            positional += 1
        if slots[slot] is not None:
            raise Reject('duplicate bound slot')
        slots[slot] = (item_index, a)
        presence[slot] = 'explicit_omit' if a.omit else 'supplied'
        if a.omit:
            if a.typ or a.value:
                raise Reject('omit must have no expression')
        else:
            evaluated_items.append(item_index)
    result = []
    for i, f in enumerate(formals):
        opt = underlying(f.typ)
        assignment = slots[i]
        if assignment is None or assignment[1].omit:
            if not optional(f, v2):
                raise Reject('required formal missing')
            if f.default is not None:
                kind, value = f.default
                if kind == 'none':
                    result.append('None')
                elif opt is not None:
                    result.append('Some(' + str(value) + ')')
                else:
                    result.append(str(value))
            elif opt is not None:
                result.append('None')
            else:
                raise Reject('non-option omitted without default')
            continue
        a = assignment[1]
        if opt is None:
            if a.typ != f.typ:
                raise Reject('scalar type mismatch')
            result.append(a.value)
        elif a.typ == f.typ:
            result.append(a.value)
        elif a.typ == opt:
            if underlying(opt) is not None:
                raise Reject('implicit nested-option lifting forbidden')
            # Legacy defaulted Option permits explicitly checked T -> Some(T).
            if not v2 and f.default is None:
                raise Reject('legacy Option required value')
            result.append('Some(' + a.value + ')')
        else:
            raise Reject('Option element type mismatch')
    return (result, presence, evaluated_items)

def expect_reject(formals, actuals, v2=True):
    try:
        bind(formals, actuals, v2)
    except Reject:
        return
    raise AssertionError('invalid case unexpectedly accepted')

def run():
    sig = [Formal('a', 'int'), Formal('b', 'Option<string>'),
           Formal('c', 'bool')]
    tests = 0
    def check(test):
        nonlocal tests
        assert test, 'reference policy assertion failed'
        tests += 1

    check(bind(sig, [Actual('3','int', 'a'),
                     Actual('true','bool','c')])[0] == ['3','None','true'])
    check(bind(sig, [Actual('3','int'), Actual(omit=True),
                     Actual('true','bool')])[2] == [0,2])
    check(bind(sig, [Actual('3','int'), Actual('None','Option<string>'),
                     Actual('true','bool')])[1][1] == 'supplied')
    check(bind(sig, [Actual('3','int'), Actual('""','string'),
                     Actual('true','bool')])[0][1] == 'Some("")')
    with_def = [Formal('a','int'), Formal('b','Option<string>', ('string','"guest"')),
                Formal('c','bool')]
    check(bind(with_def, [Actual('3','int','a'), Actual('true','bool','c')])[0][1]
          == 'Some("guest")')
    check(bind(with_def, [Actual('3','int'), Actual('None','Option<string>'),
                          Actual('true','bool')])[0][1] == 'None')
    scalar = [Formal('x','int', ('int','7')), Formal('ok','bool')]
    check(bind(scalar, [Actual('true','bool','ok')])[0] == ['7','true'])
    none_default = [Formal('x','Option<int>', ('none',None))]
    check(bind(none_default, [])[0] == ['None'])
    req = [Formal('x','Option<int>', required=True)]
    check(bind(req, [Actual('None','Option<int>')])[0] == ['None'])
    try:
        expect_reject(req, [])
        check(True)
    except AssertionError:
        raise
    check(bind(sig, [Actual('true','bool','c'),Actual('3','int','a')])[2] == [0,1])
    expect_reject(sig, [Actual('3','int')])
    check(True)
    expect_reject(sig, [Actual('3','int','a'),Actual('true','bool')])
    check(True)
    expect_reject(sig, [Actual('3','int','a'), Actual('4','int','a')])
    check(True)
    expect_reject(sig, [Actual('3','int','x'), Actual('true','bool','c')])
    check(True)
    expect_reject([Formal('x','int', ('none',None))], [])
    check(True)
    expect_reject([Formal('x','Option<int>', ('string','no'))], [])
    check(True)
    expect_reject([Formal('x','Option<int>', required=True,
                          default=('int','3'))], [])
    check(True)
    expect_reject([Formal('x','Option<Option<int>>', ('int','3'))], [])
    check(True)
    expect_reject([Formal('x','Option<Option<int>>')],
                  [Actual('None','Option<int>')])
    check(True)
    expect_reject([Formal('x','Option<int>')], [], v2=False)
    check(True)
    check(bind([Formal('x','Option<int>', ('int','4'))],
               [Actual('5','int')], v2=False)[0] == ['Some(5)'])

    # Property: any permutation of fully named, well-typed supplied formals
    # produces formal-order values, while evaluation indexes stay lexical.
    rng = random.Random(732)
    for _ in range(5000):
        n = rng.randint(1, 9)
        formals = [Formal('p'+str(i), 'int') for i in range(n)]
        order = list(range(n))
        rng.shuffle(order)
        actuals = [Actual(str(i), 'int', 'p'+str(i)) for i in order]
        values, presence, source_indexes = bind(formals, actuals)
        assert values == [str(i) for i in range(n)]
        assert source_indexes == list(range(n))
        assert presence == ['supplied'] * n
    print('PASS: %s named/default reference scenarios and 5000 source-order trials' % tests)

if __name__ == '__main__':
    run()
