//! Execute real LLVM-generated machine code, optionally comparing Java output.
use std::{
    env, fs,
    path::PathBuf,
    process::{Command, Output},
    time::{SystemTime, UNIX_EPOCH},
};
struct Scratch(PathBuf);
impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
fn run(command: &mut Command) -> Output {
    command
        .output()
        .unwrap_or_else(|error| panic!("cannot run {command:?}: {error}"))
}
fn main() {
    let (mut compiler, mut clang, mut runtime, mut java) = (None, None, None, None);
    let mut sanitize = false;
    let mut aot_driver = None;
    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--compiler" => compiler = args.next(),
            "--clang" => clang = args.next(),
            "--runtime" => runtime = args.next(),
            "--java-command" => java = args.next(),
            "--aot-driver" => aot_driver = args.next(),
            "--sanitize" => sanitize = true,
            _ => panic!("unknown argument {arg}"),
        }
    }
    let (compiler, clang, runtime) = (
        compiler.expect("--compiler"),
        clang.expect("--clang"),
        runtime.expect("--runtime"),
    );
    let scratch = Scratch(env::temp_dir().join(format!(
            "ores-class-string-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        )));
    fs::create_dir(&scratch.0).unwrap();
    let exact = include_str!("fixtures/native-class-string/main.ores");
    let foo = include_str!("fixtures/native-class-string/main-1.ores");
    let positives = [
        ("nested-multiple-children-offsets-and-return-regions", r#"define class Data as pub val Array<int> values;pub val String label;pub read():int{return self.values.get(0);}end
          define class Pair as pub val int leading;pub val Data left;pub val int gap;pub val Data right;end
          fnc make_data(int n,String label):Data{val Array<int> values=new Array<int>();values.add(n);return new Data(values,label);}
          fnc make_pair():Pair{val Data left=make_data(13,"left");val Data right=make_data(29,"right");return new Pair(7,left,11,right);}
          fnc add_data(borrow Data a,borrow Data b):int{return a.read()+b.read();}
          pub fnc main():int{val Pair pair=make_pair();return pair.leading==7&&pair.gap==11&&pair.left.label=="left"&&pair.right.label=="right"&&add_data(rt borrow pair.left,rt borrow pair.right)==42?0:1;}"#, ""),
        ("imported-nested-owned-capture", r#"import module captures from './captures.ores';pub routine main():int{return captures.verify_capture();}"#, "id\n"),
        ("nested-immutable-record-array-iteration", r#"define class Leaf as pub val int code;pub read():int{return self.code;}end define class Outer as pub val Leaf leaf;end
          define class Counter as pub let int sum=0;end
          pub fnc main():int{val Array<Outer> values=new Array<Outer>();values.add(new Outer(new Leaf(7)));values.add(new Outer(new Leaf(11)));let mut Counter count=new Counter();for(const item of values){count.sum=count.sum+item.leaf.read();}return count.sum==18?0:1;}"#, ""),
        ("nested-owned-capture-return-and-move", include_str!("fixtures/native-class-string/nested-owned-capture.ores"), "id\n"),
        ("nested-constructor-result-and-grandchild", r#"define class Data as pub val Array<int> values;pub val String label;pub read():int{return self.values.get(0);}end
          define class Envelope as pub val Data data;pub val int tag;pub read():int{return self.data.read();}end
          define class Root as pub val Envelope envelope;pub read():int{return self.envelope.read();}end
          fnc make_envelope():Envelope{val Array<int> values=new Array<int>();values.add(42);val Data data=new Data(values,"nested");return new Envelope(data,7);}
          fnc make_root():Root{val Envelope envelope=make_envelope();return new Root(envelope);}
          fnc read_data(borrow Data data):int{return data.read();}
          pub routine main():int{val Root root=make_root();return root.read()==42&&read_data(rt borrow root.envelope.data)==42&&root.envelope.tag==7&&root.envelope.data.label=="nested"?0:1;}"#, ""),
        ("nested-projection-exclusively-mutates-array", r#"define class Data as pub val Array<int> values;pub edit(mut self)(int value):void{self.values.set(0,value);return;}end
          define class Outer as pub val Data data;end
          pub routine main():int{val Array<int> values=new Array<int>();values.add(1);val Data data=new Data(values);val Outer outer=new Outer(data);outer.data.edit(42);return outer.data.values.get(0)==42?0:1;}"#, ""),
        ("nested-immutable-scalar-result", r#"define class Leaf as pub val String name;pub read():String{return self.name;}end
          define class Outer as pub val Leaf leaf;pub read():String{return self.leaf.read();}end
          fnc make_outer():Outer{val Leaf leaf=new Leaf("value");return new Outer(leaf);}
          pub routine main():int{val Outer outer=make_outer();return outer.read()=="value"&&outer.leaf.name=="value"?0:1;}"#, ""),
        ("imported-module-routing-validation", r#"import module paths from './paths.ores';pub routine main():int{return paths.verify_paths();}"#, ""),
        ("void-structured-return-runs-defer", r#"define class Counter as pub let int value=0;end
          fnc update(borrow mut Counter counter):void{defer counter.value=99;for(let int index=0;index<2;index=index+1){if index==1;then return;fi}return;}
          pub fnc main():int{let mut Counter counter=new Counter();update(rt borrow mut counter);return counter.value==99?0:1;}"#, ""),
        ("foreach-return-releases-loan-before-defer", r#"define class Box as pub val Array<int> values;pub let int code=7;end
          fnc pick(borrow mut Box owner):int{defer owner.code=99;for(const item of owner.values){return owner.code;}return -1;}
          pub fnc main():int{val Array<int> values=new Array<int>();values.add(1);val Box owner=new Box(values);val selected:int=pick(rt borrow mut owner);return selected==7&&owner.code==99?0:1;}"#, ""),
        ("index-loop-branch-exits-and-string-result", r#"fnc choose_text(bool first):String{for(let int index=0;index<1;index=index+1){if(first){val String result="yes";return result;}else{val String result="no";return result;}fi}return "empty";}
          pub fnc main():int{return choose_text(true)=="yes"&&choose_text(false)=="no"?0:1;}"#, ""),
        ("index-loop-update-order-and-zero-iterations", r#"define class Counter as pub let int value=0;end
          pub fnc main():int{let mut Counter counter=new Counter();for(let int index=0;index<4;index=index+1){counter.value=counter.value+index;}
          for(let int index=4;index<4;index=index+1){counter.value=999;}return counter.value==6?0:1;}"#, ""),
        ("actual-routing-path-validation", include_str!("fixtures/native-class-string/routing-path-validation.ores"), ""),
        ("foreach-return-unwinds-scopes", r#"fnc first(bool fill):int{val Array<int> values=new Array<int>();if(fill){values.add(42);}else{}fi for(const item of values){return item;}return 0;}pub fnc main():int{return first(true)==42&&first(false)==0?0:1;}"#, ""),
        ("index-return-snapshot-before-deferred-field-write", r#"define class Counter as pub let int value=7;end
          fnc choose(borrow mut Counter counter):int{defer counter.value=99;for(let int index=0;index<3;index=index+1){if index==1;then val int selected=counter.value;return selected;fi}return -1;}
          pub fnc main():int{let mut Counter counter=new Counter();val selected:int=choose(rt borrow mut counter);return selected==7&&counter.value==99?0:1;}"#, ""),
        ("nested-read-loans-release-after-loop", r#"define class Counter as pub let int total=0;end
          pub routine main():int{val Array<int> values=new Array<int>();values.add(1);values.add(2);let mut Counter counter=new Counter();
          for(const first of values){for(const second of values){counter.total=counter.total+first+second;}}values.add(3);return counter.total==12&&values.size==3?0:1;}"#, ""),
        ("typed-router-copy-concat-iteration", include_str!("fixtures/native-class-string/typed-array-iteration.ores"), "foo\nid\n"),
        ("string-array-copy-empty-and-nul", r#"define class RequestPath as
          pub val Array<String> segments;
          pub copy():RequestPath{val Array<String> copied=new Array<String>();for(const segment of self.segments){copied.add(segment);}return new RequestPath(copied);}
          end pub routine main():int{val Array<String> a=new Array<String>();a.add("x\u0000y");a.add("é");val RequestPath p=new RequestPath(a);val RequestPath q=p.copy();
          val Array<String> empty=new Array<String>();val RequestPath e=new RequestPath(empty);val RequestPath f=e.copy();
          return q.segments.get(0)=="x\u0000y"&&q.segments.get(1)=="é"&&f.segments.size==0?0:1;}"#, ""),
        ("nested-iteration-scopes-and-branches", r#"define class Item as pub val int value;pub read():int{return self.value;}end
          define class Counter as pub let int value=0;end
          pub routine main():int{val Array<Item> a=new Array<Item>();a.add(new Item(2));a.add(new Item(3));
          val Array<int> b=new Array<int>();b.add(10);b.add(20);let mut Counter count=new Counter();
          for(const item of a){val Item view=a.get(0);for(const n of b){if(n==10){val int amount=view.read()+n;count.value=count.value+amount;}else{val int amount=item.value+n;count.value=count.value+amount;}fi}}
          b.add(30);return count.value==69&&b.size==3?0:1;}"#, ""),

        ("exact-user-program", exact, "hello world\nhiiii 2hiiii"),
        ("java-two-argument-println",
            r#"import class Foo from './main-1.ores';
               pub routine main():int {
                 stdio.println("hello world");
                 val Foo f = new Foo();
                 val string x = f.bar();
                 stdio.println("x", x);
                 return 0;
               }"#,
            "hello world\nxhiiii 2hiiii\n"),
        ("stdio-effect-order",
            r#"fnc mark(string s):string{stdio.print(s);return s;}
               pub routine main():int{stdio.println(mark("a"), mark("b"), "c");return 0;}"#,
            "ababc\n"),
        ("stdio-utf8-and-nul",
            r#"pub routine main():int{stdio.print("A", "\u0000", "é");return 0;}"#,
            "A\0é"),
        ("stdio-32-args",
            r#"pub fnc main():int{stdio.println("x","x","x","x","x","x","x","x","x","x","x","x","x","x","x","x","x","x","x","x","x","x","x","x","x","x","x","x","x","x","x","x");return 0;}"#,
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n"),

        (
            "imported-owning-class-return",
            r#"import class Box from './owned-box.ores';
              fnc make_box():Box{val Array<int> values=new Array<int>();values.add(42);return new Box(values);}
              pub routine main():int{val Box value=make_box();val Box copied=value.copy();return value.values.get(0)==42&&copied.values.get(0)==42?0:1;}"#,
            "",
        ),
        (
            "owning-mutable-aggregate-call-result-and-move",
            r#"define class Shelf as pub let int tag=1;pub let mut Array<int> first=new Array<int>();pub let mut Array<int> second=new Array<int>();end
              fnc make_shelf():Shelf{let mut Shelf value=new Shelf();value.first.add(13);value.second.add(29);return value;}
              fnc edit(borrow mut Shelf value):void{value.first.set(0,14);return;}
              pub routine main():int{val Shelf value=true?make_shelf():make_shelf();let mut Shelf moved=rt take value;edit(rt borrow mut moved);return moved.tag+moved.first.get(0)+moved.second.get(0)==44?0:1;}"#,
            "",
        ),
        (
            "owning-string-array-constructor-conditional-return",
            r#"define class Names as pub val Array<String> values;end
              fnc make_names(bool first):Names{val Array<String> values=new Array<String>();values.add("native");return first?new Names(values):new Names(rt take values);}
              pub routine main():int{val Names a=make_names(true);val Names b=make_names(false);stdio.println(a.values.get(0));return b.values.get(0)=="native"?0:1;}"#,
            "native\n",
        ),
        (
            "owned-aggregate-return-and-forwarding",
            include_str!("fixtures/native-class-string/owned-aggregate-return.ores"),
            "/foo/:id\nid\n",
        ),
        (
            "imported-class-return-and-borrowed-method",
            r#"import class Foo from './main-1.ores';
              fnc create():Foo{return new Foo();}
              pub routine main():int{val Foo value=create();stdio.println(value.bar());return 0;}"#,
            "hiiii 2hiiii\n",
        ),
        (
            "lazy-record-return-forwarding",
            r#"define class Value as pub val int code;end
              fnc fresh():Value{return new Value(42);}
              fnc choose_record(bool use_new):Value{return use_new?new Value(42):fresh();}
              fnc same_owner(bool first):Value{val Value value=new Value(42);return first?value:value;}
              pub routine main():int{val Value a=choose_record(true);val Value b=choose_record(false);val Value c=same_owner(true);val Value d=same_owner(false);return a.code==42&&b.code==42&&c.code==42&&d.code==42?0:1;}"#,
            "",
        ),
        (
            "immutable-record-returns-after-callee-cleanup",
            include_str!("fixtures/native-class-string/record-return.ores"),
            "segment\nlocal\n",
        ),
        (
            "owned-path-pattern-borrowed-methods",
            include_str!("fixtures/native-class-string/owned-path-pattern.ores"),
            "/foo/:id\nfoo\n",
        ),
        (
            "nested-borrows-distinguish-array-regions",
            r#"define class Box as pub val Array<int> values; end
              fnc sum(borrow Box left,borrow Box right):int{return left.values.get(0)+right.values.get(0);}
              fnc nested(borrow Box parent):int {
                val Array<int> numbers=new Array<int>(); numbers.add(29);
                val Box child=new Box(numbers);
                return sum(rt borrow parent,rt borrow child);
              }
              pub routine main():int {
                val Array<int> numbers=new Array<int>(); numbers.add(13);
                val Box owner=new Box(numbers);
                return nested(rt borrow owner)==42?0:1;
              }"#,
            "",
        ),
        (
            "exclusive-class-array-borrow",
            r#"define class Box as pub val Array<int> values; end
              fnc append(borrow mut Box owner):void {owner.values.add(29);return;}
              pub routine main():int {
                val Array<int> numbers=new Array<int>(); numbers.add(13);
                val Box owner=new Box(rt take numbers);
                append(rt borrow mut owner);
                return owner.values.size==2 && owner.values.get(1)==29?0:1;
              }"#,
            "",
        ),
        (
            "borrowed-string-array-field",
            r#"define class Names as val Array<String> values;
                pub first():String{return self.values.get(0);}
              end
              pub routine main():int {
                val Array<String> names=new Array<String>();names.add("route");
                val Names owner=new Names(names);
                stdio.println(owner.first());return 0;
              }"#,
            "route\n",
        ),
        (
            "immutable-record-array-replacement",
            include_str!("fixtures/native-class-string/record-array.ores"),
            "foo\ntail\n",
        ),
        (
            "imported-record-array-borrowed-method",
            r#"import class Foo from './main-1.ores';
              pub routine main():int {
                val Array<Foo> values=new Array<Foo>();
                values.add(new Foo());
                val Foo first=values.get(0);
                stdio.println(first.bar());
                return 0;
              }"#,
            "hiiii 2hiiii\n",
        ),
        (
            "routing-path-segment-record",
            include_str!("fixtures/native-class-string/path-segment.ores"),
            "foo\n",
        ),
        (
            "typed-bool-and-string-record",
            r#"define class Header as pub val bool enabled; pub val String text; end
              pub routine main():int {
                val Header header=new Header(true,"Content-Type");
                stdio.println(header.text);
                return header.enabled && header.text == "Content-Type" ? 0 : 1;
              }"#,
            "Content-Type\n",
        ),
        (
            "string-field-literal-initializer",
            r#"define class Label as pub val String text="ready"; end
              pub routine main():int {val Label label=new Label();stdio.println(label.text);return 0;}"#,
            "ready\n",
        ),
        (
            "typed-string-array-path-segments",
            include_str!("fixtures/native-class-string/string-array.ores"),
            "foo\nbaz\n",
        ),
        (
            "string-array-retains-immutable-values",
            r#"pub routine main():int {
                let mut Array<string> values = new Array<string>();
                values.add("a\u0000b");
                values.add(values.get(0));
                values.set(0, "changed");
                return values.size == 2 && values.get(1) == "a"+"\u0000b" && values.get(0) == "changed" ? 0 : 1;
              }"#,
            "",
        ),
        (
            "string-content-equality",
            r#"fnc same(string a,string b):bool{return a == b;}
              pub routine main():int {
                val string a="foo"; val string b="f"+"oo";
                stdio.println(same(a,b) ? "equal" : "WRONG");
                stdio.println(a != "bar" ? "different" : "WRONG");
                stdio.println("" == "" ? "empty" : "WRONG");
                return 0;
              }"#,
            "equal\ndifferent\nempty\n",
        ),
        (
            "string-equality-embedded-nul-and-unicode",
            r#"pub routine main():int {
                stdio.println("a\u0000b" == "a"+"\u0000b" ? "nul" : "WRONG");
                stdio.println("a\u0000b" != "a" ? "length" : "WRONG");
                stdio.println("\u00e9" == "é" ? "unicode" : "WRONG");
                return 0;
              }"#,
            "nul\nlength\nunicode\n",
        ),
        (
            "http-string-routing-primitives",
            r#"fnc route(string method,string path):int {
                if (method != "GET") { return 405; } fi
                return path == "/health" ? 200 : 404;
              }
              pub routine main():int {
                stdio.println(route("G"+"ET", "/"+"health") == 200 ? "health" : "WRONG");
                stdio.println(route("GET", "/missing") == 404 ? "missing" : "WRONG");
                stdio.println(route("POST", "/health") == 405 ? "method" : "WRONG");
                return 0;
              }"#,
            "health\nmissing\nmethod\n",
        ),
        (
            "source-effect-order",
            r#"fnc mark():string{stdio.print("method;");return "a";}
          pub fnc main():int{stdio.print("before;");val string a=mark();stdio.print("after;");
          val string b=mark();stdio.print(a+b);return 0;}"#,
            "before;method;after;method;aa",
        ),
        (
            "parameters-and-return",
            r#"fnc join(string a,string b):string{return a+b;}
          pub fnc main():int{val string a=join("left", "right");stdio.println(a);return 0;}"#,
            "leftright\n",
        ),
        (
            "lazy-conditional",
            r#"fnc fail():string{stdio.print("WRONG");return "wrong";}
          pub fnc main():int{val string x=true ? "yes" : fail();stdio.print(x);return 0;}"#,
            "yes",
        ),
        (
            "escaped-unicode",
            r#"pub fnc main():int{stdio.print("\u00e9\uD83D\uDE00\n\t\"\\");return 0;}"#,
            "é😀\n\t\"\\",
        ),
        (
            "empty-and-embedded-nul",
            r#"pub fnc main():int{val string x=""+"a\u0000b";stdio.print(x+"");return 0;}"#,
            "a\0b",
        ),
    ];
    // Java does not yet admit named value arguments. Keep that ABI regression
    // explicit and native-only rather than weakening the common-source gate.
    let named_source = positives.iter().find(|case| case.0 == "nested-borrows-distinguish-array-regions").unwrap().1.replace(
        "sum(rt borrow parent,rt borrow child)",
        "sum(right:rt borrow child,left:rt borrow parent)",
    );
    // The pinned Java parser still requires an initializer for final class fields.
    let native_only = [
        ("named-borrows-distinguish-array-regions", named_source.as_str(), ""),
        ("readonly-nested-child-retains-read-access", r#"define class Data as pub val Array<int> values;pub read():int{return self.values.get(0);}end define class Outer as pub final Data child;end
          pub fnc main():int{val Array<int> values=new Array<int>();values.add(42);val Data child=new Data(values);val Outer outer=new Outer(child);return outer.child.read()==42&&outer.child.values.size==1?0:1;}"#, ""),
    ];
    let mut comparisons = 0;
    let mut guest_address_checks = false;
    for (case_index, (name, source, expected)) in positives.iter().chain(native_only.iter()).enumerate() {
        fs::write(scratch.0.join("main.ores"), source).unwrap();
        fs::write(scratch.0.join("main-1.ores"), foo).unwrap();
        fs::write(scratch.0.join("owned-box.ores"), "define pub class Box as pub val Array<int> values;pub copy():Box{val Array<int> copied=new Array<int>();copied.add(self.values.get(0));return new Box(copied);}end").unwrap();
        let captures_fixture = include_str!("fixtures/native-class-string/nested-owned-capture.ores")
            .replace("pub routine main():int", "pub fnc verify_capture():int");
        fs::write(scratch.0.join("captures.ores"), format!("define module captures as\n{captures_fixture}\nend\n")).unwrap();
        let module_fixture = include_str!("fixtures/native-class-string/routing-path-validation.ores")
            .replace("pub routine main(): int", "pub fnc verify_paths(): int");
        fs::write(scratch.0.join("paths.ores"), format!("define module paths as\n{module_fixture}\nend\n")).unwrap();
        let path = scratch.0.join("main.ores");
        let generated = run(Command::new(&compiler).arg(&path));
        assert!(
            generated.status.success() && !generated.stdout.is_empty(),
            "{name}: {}",
            String::from_utf8_lossy(&generated.stderr)
        );
        // Scope slots inside repeated control flow must never allocate per
        // iteration. Verify the generated CFG before linking, even at O0.
        let mut block = "";
        for line in std::str::from_utf8(&generated.stdout).unwrap().lines() {
            if let Some((label, _)) = line.split_once(':') {
                if !line.starts_with(' ') { block = label; }
            }
            if [".arm.stack", "record.fields", "record.argument"].iter().any(|name| line.contains(name)) && line.contains("alloca ") {
                assert_eq!(block, "entry", "{name}: scope slot allocated on a control-flow edge");
            }
        }
        let ir = scratch.0.join("program.ll");
        let binary = scratch.0.join("program");
        let raw_ir = std::str::from_utf8(&generated.stdout).unwrap();
        // Clang's -fsanitize flag alone does not mark imported LLVM IR
        // functions for ASan. Mark every definition, then verify that actual
        // guest-memory check calls are emitted (not just runtime linking).
        let checked_ir = if sanitize {
            raw_ir.lines().map(|line| {
                if line.starts_with("define ") {
                    format!("{} sanitize_address {{\n", line.strip_suffix('{').expect("function definition"))
                } else { format!("{line}\n") }
            }).collect::<String>()
        } else { raw_ir.to_owned() };
        fs::write(&ir, checked_ir).unwrap();
        if sanitize && *name == "nested-constructor-result-and-grandchild" {
            let checked = run(Command::new(&clang).args(["-O1", "-S", "-emit-llvm", "-fsanitize=address", "-x", "ir"])
                .arg(&ir).args(["-o", "-"]));
            assert!(checked.status.success(), "guest ASan: {}", String::from_utf8_lossy(&checked.stderr));
            let instrumented = String::from_utf8_lossy(&checked.stdout);
            assert!(["call void @__asan_report_load8", "call void @__asan_report_store8",
                "call void @__asan_report_load_n", "call void @__asan_report_store_n"].iter().any(|call| instrumented.contains(call)),
                "guest LLVM memory accesses were not instrumented");
            guest_address_checks = true;
        }
        if binary.exists() {
            fs::remove_file(&binary).unwrap();
        }
        let mut link = Command::new(&clang);
        link.args(["-O1", "-g"]);
        if sanitize {
            link.args([
                "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer",
            ]);
        }
        let linked = run(link
            .arg("-x")
            .arg("ir")
            .arg(&ir)
            .args(["-x", "c", "-std=c11"])
            .arg("-I")
            .arg(PathBuf::from(&runtime).parent().unwrap().parent().unwrap().parent().unwrap())
            .arg(&runtime)
            .arg(PathBuf::from(&runtime).parent().unwrap().join("NativeIntArrayRegion.c"))
            .arg(PathBuf::from(&runtime).parent().unwrap().join("NativeRecordRegion.c"))
            .arg("-pthread")
            .arg("-o")
            .arg(&binary));
        assert!(
            linked.status.success() && binary.is_file(),
            "{name}: {}",
            String::from_utf8_lossy(&linked.stderr)
        );
        for _ in 0..5 {
            let native = run(&mut Command::new(&binary));
            assert_eq!(
                native.status.code(),
                Some(0),
                "{name}: {}",
                String::from_utf8_lossy(&native.stderr)
            );
            assert_eq!(native.stdout, expected.as_bytes(), "{name}: native output");
            assert!(
                native.stderr.is_empty(),
                "{name}: sanitizer/runtime diagnostics: {}",
                String::from_utf8_lossy(&native.stderr)
            );
        }
        if let Some(launcher) = java.as_ref().filter(|_| case_index < positives.len()) {
            let reference = run(Command::new(launcher).arg(&path));
            assert_eq!(
                reference.status.code(),
                Some(0),
                "Java {name}: {}",
                String::from_utf8_lossy(&reference.stderr)
            );
            assert_eq!(reference.stdout, expected.as_bytes(), "Java {name}: output");
            comparisons += 1;
        }
    }
    let mut standalone_runs = 0;
    if let Some(driver) = &aot_driver {
        #[cfg(unix)] {
            use std::os::unix::{fs::PermissionsExt, fs::symlink};
            let source = scratch.0.join("protected.ores");
            fs::write(&source, "pub fnc main():int{return 0;}").unwrap();
            let fake_compiler = scratch.0.join("compiler");
            let fake_clang = scratch.0.join("clang");
            let sentinel = b"#!/bin/sh\necho 'must not execute' >&2\nexit 99\n";
            for path in [&fake_compiler, &fake_clang] {
                fs::write(path, sentinel).unwrap();
                fs::set_permissions(path, fs::Permissions::from_mode(0o755)).unwrap();
            }
            let symbolic = scratch.0.join("compiler-symlink");
            symlink(&fake_compiler, &symbolic).unwrap();
            let hard = scratch.0.join("clang-hardlink");
            fs::hard_link(&fake_clang, &hard).unwrap();
            for destination in [&fake_compiler, &fake_clang, &symbolic, &hard] {
                let rejected = run(Command::new(driver).env("ORESLANG_LLVMC", &fake_compiler)
                    .env("CLANG", &fake_clang).arg(&source).arg(destination));
                assert_eq!(rejected.status.code(), Some(64), "AOT alias guard: {}", String::from_utf8_lossy(&rejected.stderr));
                assert!(String::from_utf8_lossy(&rejected.stderr).contains("must not alias"));
                assert_eq!(fs::read(destination).unwrap(), sentinel);
            }
        }
        let scalar_record = r#"define class Number as pub val int code;end
          fnc create():Number{return new Number(42);}
          pub routine main():int{val Number value=create();return value.code==42?0:1;}"#;
        let owning_int = r#"define class Box as pub val Array<int> values;end
          fnc create():Box{val Array<int> a=new Array<int>();a.add(42);return new Box(a);}
          pub routine main():int{val Box value=create();return value.values.get(0)==42?0:1;}"#;
        for (source, expected) in [
            (include_str!("fixtures/native-class-string/nested-owned-capture.ores"), "id\n"),
            (include_str!("fixtures/native-class-string/routing-path-validation.ores"), ""),
            (include_str!("fixtures/native-class-string/typed-array-iteration.ores"), "foo\nid\n"),
            (scalar_record, ""),
            (owning_int, ""),
            (include_str!("fixtures/native-class-string/record-return.ores"), "segment\nlocal\n"),
            (include_str!("fixtures/native-class-string/owned-aggregate-return.ores"), "/foo/:id\nid\n"),
        ] {
            let path = scratch.0.join("standalone.ores");
            let binary = scratch.0.join("standalone");
            fs::write(&path, source).unwrap();
            let built = run(Command::new(driver)
                .env("ORESLANG_LLVMC", fs::canonicalize(&compiler).unwrap())
                .env("CLANG", &clang).arg(&path).arg(&binary));
            assert!(built.status.success(), "AOT driver: {}", String::from_utf8_lossy(&built.stderr));
            fs::remove_file(&path).unwrap();
            let executed = run(Command::new(&binary).env_clear().env("PATH", ""));
            assert_eq!(executed.status.code(), Some(0), "source-free AOT: {}", String::from_utf8_lossy(&executed.stderr));
            assert_eq!(executed.stdout, expected.as_bytes());
            assert!(executed.stderr.is_empty());
            standalone_runs += 1;
        }
    }
    let negatives = [
        ("stdio-mixed-int-string",
            r#"pub fnc main():int{stdio.println("a",2);return 0;}"#.to_owned(), foo.to_owned()),
        ("stdio-33-args",
            format!("pub fnc main():int{{stdio.println({});return 0;}}",
                    vec!["\"x\""; 33].join(",")), foo.to_owned()),

        ("nested-explicit-constructor-cannot-reinterpret-scalar", r#"define class Child as pub val int value;end define class Outer as val Child child;constructor(int child){self.child=child;}end pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-depth-forward-order", format!("{}pub fnc main():int{{return 0;}}", (0..10).map(|i| if i==9 { "define class Level9 as val int value;end".to_owned() } else { format!("define class Level{i} as val Level{} child;end",i+1) }).collect::<String>()), foo.to_owned()),
        ("nested-depth-reverse-order", format!("{}pub fnc main():int{{return 0;}}", (0..10).rev().map(|i| if i==9 { "define class Level9 as val int value;end".to_owned() } else { format!("define class Level{i} as val Level{} child;end",i+1) }).collect::<String>()), foo.to_owned()),
        ("nested-expanded-layout-bound", format!("define class Child as {}end define class Outer as val Child child;val int extra;end pub fnc main():int{{return 0;}}", (0..16).map(|i| format!("val int field_{i};")).collect::<String>()), foo.to_owned()),
        ("final-primitive-projection-cannot-borrow-mut", r#"define class Counter as pub val int value;end fnc observe(borrow mut int value):int{return value;}pub fnc main():int{val Counter counter=new Counter(1);return observe(rt borrow mut counter.value);}"#.to_owned(), foo.to_owned()),
        ("readonly-child-edge-cannot-borrow-mut", r#"define class Child as pub val Array<int> values;end define class Outer as pub final Child child;end fnc edit(borrow mut Child child):void{child.values.add(1);return;}pub fnc main():int{val Array<int> values=new Array<int>();val Child child=new Child(values);val Outer outer=new Outer(child);edit(rt borrow mut outer.child);return 0;}"#.to_owned(), foo.to_owned()),
        ("readonly-child-edge-cannot-mutate-array", r#"define class Child as pub val Array<int> values;end define class Outer as pub final Child child;end pub fnc main():int{val Array<int> values=new Array<int>();val Child child=new Child(values);val Outer outer=new Outer(child);outer.child.values.add(1);return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-duplicate-child-owner", r#"define class Child as pub val Array<int> values;end define class Pair as val Child first;val Child second;end pub fnc main():int{val Array<int> a=new Array<int>();val Child child=new Child(a);val Pair pair=new Pair(child,child);return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-child-use-after-move", r#"define class Child as pub val Array<int> values;end define class Outer as val Child child;end pub fnc main():int{val Array<int> a=new Array<int>();val Child child=new Child(a);val Outer outer=new Outer(child);return child.values.size;}"#.to_owned(), foo.to_owned()),
        ("nested-borrowed-child-consumption", r#"define class Child as pub val Array<int> values;end define class Outer as val Child child;end fnc bad(borrow Child child):int{val Outer outer=new Outer(child);return 0;}pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-wrong-nominal-child", r#"define class A as pub val int value;end define class B as pub val int value;end define class Outer as val A child;end pub fnc main():int{val B child=new B(1);val Outer outer=new Outer(child);return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-integer-forged-child", r#"define class Child as pub val int value;end define class Outer as val Child child;end pub fnc main():int{val Outer outer=new Outer(1);return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-parent-private-edge", r#"define class Child as pub val int value;end define class Outer as val Child child;end pub fnc main():int{val Child child=new Child(1);val Outer outer=new Outer(child);return outer.child.value;}"#.to_owned(), foo.to_owned()),
        ("nested-child-private-edge", r#"define class Child as val int value;end define class Outer as pub val Child child;pub leak():int{return self.child.value;}end pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-private-child-loan", r#"define class Child as pub val int value;end define class Outer as val Child child;end fnc read(borrow Child child):int{return child.value;}pub fnc main():int{val Child child=new Child(1);val Outer outer=new Outer(child);return read(rt borrow outer.child);}"#.to_owned(), foo.to_owned()),
        ("nested-shared-to-exclusive-escalation", r#"define class Child as pub val Array<int> values;end define class Outer as pub val Child child;end fnc edit(borrow mut Child child):void{child.values.add(1);return;}fnc bad(borrow Outer outer):void{edit(rt borrow mut outer.child);return;}pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-projection-alias-conflict", r#"define class Child as pub val int value;end define class Outer as pub val Child child;end fnc pair(borrow mut Child a,borrow Child b):int{return b.value;}pub fnc main():int{val Child child=new Child(1);val Outer outer=new Outer(child);return pair(rt borrow mut outer.child,rt borrow outer.child);}"#.to_owned(), foo.to_owned()),
        ("nested-owning-field-cannot-escape-by-value", r#"define class Child as pub val Array<int> values;end define class Outer as pub val Child child;pub leak():Child{return self.child;}end pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-cycle", r#"define class Node as val Node child;end pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-indirect-cycle", r#"define class A as val B child;end define class B as val A child;end pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-mutable-child-slot", r#"define class Child as pub let int value=1;end define class Outer as val Child child;end pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-fieldless-identity", r#"define class Empty as end define class Outer as val Empty child;end pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-unknown-child-type", r#"define class Outer as val Missing child;end pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),

        ("index-loop-shadows-live-local", r#"pub fnc main():int{val index:int=0;for(let int index=0;index<1;index=index+1){}return 0;}"#.to_owned(), foo.to_owned()),
        ("index-counter-escape", r#"pub fnc main():int{for(let int index=0;index<1;index=index+1){}return index;}"#.to_owned(), foo.to_owned()),
        ("index-initializer-wrong-type", r#"pub fnc main():int{for(let int index="bad";index<1;index=index+1){}return 0;}"#.to_owned(), foo.to_owned()),
        ("index-condition-wrong-type", r#"pub fnc main():int{for(let int index=0;index+1;index=index+1){}return 0;}"#.to_owned(), foo.to_owned()),
        ("index-update-wrong-type", r#"pub fnc main():int{for(let int index=0;index<1;index=true){}return 0;}"#.to_owned(), foo.to_owned()),
        ("index-updates-other-counter", r#"pub fnc main():int{val other:int=0;for(let int index=0;index<1;other=index+1){}return 0;}"#.to_owned(), foo.to_owned()),
        ("index-backedge-consumes-outer", r#"fnc consume(int n):void{return;}pub fnc main():int{val external_value:int=42;for(let int index=0;index<2;index=index+1){consume(rt take external_value);}return 0;}"#.to_owned(), foo.to_owned()),
        ("index-condition-consumes-counter", r#"fnc yes(int n):bool{return true;}pub fnc main():int{for(let int index=0;yes(rt take index);index=index+1){}return 0;}"#.to_owned(), foo.to_owned()),
        ("structured-return-wrong-type", r#"pub fnc main():int{for(let int index=0;index<1;index=index+1){return true;}return 0;}"#.to_owned(), foo.to_owned()),
        ("structured-return-ownership-transfer", r#"pub fnc main():int{val external_value:int=42;for(let int index=0;index<1;index=index+1){return rt take external_value;}return 0;}"#.to_owned(), foo.to_owned()),
        ("late-defer-after-conditional-return", r#"fnc cleanup():void{return;}pub fnc main():int{for(let int index=0;index<1;index=index+1){return 0;}defer cleanup();return 0;}"#.to_owned(), foo.to_owned()),
        ("structured-owning-class-return", r#"define class Box as pub val Array<int> values;end fnc bad():Box{val Array<int> values=new Array<int>();val Box box=new Box(values);for(let int index=0;index<1;index=index+1){return box;}return box;}pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),

        ("iteration-backedge-consumes-scalar-owner", r#"fnc consume(int value):void{return;}pub fnc main():int{val Array<int> values=new Array<int>();values.add(1);val outer:int=42;for(const item of values){consume(rt take outer);}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-record-alias-without-borrow", r#"define class Item as pub val int value;end pub fnc main():int{val Array<Item> a=new Array<Item>();for(const item of a){val Item other=item;}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-mutates-source", r#"pub fnc main():int{val Array<int> a=new Array<int>();a.add(1);for(const n of a){a.add(n);}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-mutates-source-field", r#"define class Box as pub val Array<int> a;end pub fnc main():int{val Array<int> a=new Array<int>();a.add(1);val Box b=new Box(a);for(const n of b.a){b.a.set(0,n);}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-exclusive-owner-helper", r#"define class Box as pub val Array<int> a;end fnc edit(borrow mut Box b):void{b.a.add(1);return;}pub fnc main():int{val Array<int> a=new Array<int>();a.add(1);val Box b=new Box(a);for(const n of b.a){edit(rt borrow mut b);}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-consumes-outer-owner", r#"define class Box as pub val Array<int> a;end fnc consume(Box b):void{return;}pub fnc main():int{val Array<int> a=new Array<int>();a.add(1);val Array<int> b=new Array<int>();val Box other=new Box(b);for(const n of a){consume(rt take other);}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-element-scope-escape", r#"pub fnc main():int{val Array<int> a=new Array<int>();for(const n of a){}return n;}"#.to_owned(), foo.to_owned()),
        ("iteration-local-scope-escape", r#"pub fnc main():int{val Array<int> a=new Array<int>();for(const n of a){val int x=n;}return x;}"#.to_owned(), foo.to_owned()),
        ("iteration-element-shadow", r#"pub fnc main():int{val Array<int> a=new Array<int>();val int n=0;for(const n of a){}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-nested-shadow", r#"pub fnc main():int{val Array<int> a=new Array<int>();for(const n of a){for(const n of a){}}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-mutable-record", r#"define class Item as pub let int value=1;end pub fnc main():int{val Array<Item> a=new Array<Item>();for(const n of a){stdio.println(n.value);}return 0;}"#.to_owned(), foo.to_owned()),
        ("scoped-mutable-record-snapshot", r#"define class Item as pub let int value=1;end pub fnc main():int{val Item i=new Item();if(true){val Item j=i;}else{}fi return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-element-write", r#"define class Item as pub val int value;end pub fnc main():int{val Array<Item> a=new Array<Item>();for(const n of a){n.value=2;}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-break-without-cleanup", r#"pub fnc main():int{val Array<int> a=new Array<int>();for(const n of a){break;}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-non-array-source", r#"pub fnc main():int{val int a=1;for(const n of a){}return 0;}"#.to_owned(), foo.to_owned()),

        ("owning-return-duplicate-array", "define class Pair as val Array<int> a;val Array<int> b;end fnc bad():Pair{val Array<int> values=new Array<int>();return new Pair(values,values);}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("owning-return-borrowed-field", "define class Box as pub val Array<int> values;end fnc bad(borrow Box value):Box{return new Box(value.values);}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("owning-return-deferred-source-reuse", "define class Box as pub val Array<int> values;end fnc observe_owner(borrow Box value):void{return;}fnc bad():Box{val Array<int> a=new Array<int>();val Box value=new Box(a);defer observe_owner(rt borrow value);return value;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("owning-result-use-after-class-move", "define class Box as pub val Array<int> values;end fnc make():Box{val Array<int> a=new Array<int>();return new Box(a);}pub fnc main():int{val Box a=make();let mut Box b=rt take a;return a.values.size;}".to_owned(), foo.to_owned()),
        ("owning-result-by-value-call", "define class Box as pub val Array<int> values;end fnc make():Box{val Array<int> a=new Array<int>();return new Box(a);}fnc observe_owner(Box value):int{return value.values.size;}pub fnc main():int{val Box b=make();return observe_owner(b);}".to_owned(), foo.to_owned()),
        ("record-return-deferred-owner-reuse", "define class Item as pub val int code;end fnc observe(borrow Item value):void{return;}fnc bad(bool first):Item{val Item value=new Item(42);defer observe(rt borrow value);return first?value:value;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("record-return-integer-forgery", "define class Item as pub val int code;end fnc bad():Item{return 42;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("record-return-wrong-nominal", "define class A as pub val int code;end define class B as pub val int code;end fnc bad():A{return new B(1);}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("record-return-mutable-identity", "define class Item as pub let int code=42;end fnc bad():Item{return new Item();}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("record-return-borrowed-array-owner", "define class Box as val Array<int> values;end fnc bad(borrow Box value):Box{return value;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("record-return-incoming-borrow", "define class Item as pub val int code;end fnc bad(borrow Item value):Item{return value;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("record-return-moved-local", "define class Item as pub val int code;end fnc bad():Item{let mut Item value=new Item(1);let mut Item moved=rt take value;return value;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("record-return-not-os-status", "define class Item as pub val int code;end pub fnc main():Item{return new Item(42);}".to_owned(), foo.to_owned()),
        ("record-return-handle-as-int", "define class Item as pub val int code;end fnc make():Item{return new Item(42);}pub fnc main():int{return make();}".to_owned(), foo.to_owned()),
        ("record-return-unknown-type", "fnc bad():Missing{return 0;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("constructor-array-use-after-move", "define class Box as val Array<int> values; end pub fnc main():int{val Array<int> a=new Array<int>();val Box b=new Box(a);return a.size;}".to_owned(), foo.to_owned()),
        ("constructor-array-duplicate-owner", "define class Pair as val Array<int> left;val Array<int> right;end pub fnc main():int{val Array<int> a=new Array<int>();val Pair p=new Pair(a,a);return 0;}".to_owned(), foo.to_owned()),
        ("constructor-array-element-mismatch", "define class Box as val Array<String> values;end pub fnc main():int{val Array<int> a=new Array<int>();val Box b=new Box(a);return 0;}".to_owned(), foo.to_owned()),
        ("shared-class-array-mutation", "define class Box as pub val Array<int> values;end fnc bad(borrow Box owner):void{owner.values.add(1);return;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("constructor-array-borrowed-field", "define class Box as pub val Array<int> values;end fnc bad(borrow Box owner):int{val Box other=new Box(owner.values);return 0;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("class-array-by-value-escape", "define class Box as pub val Array<int> values;end fnc bad(Box owner):int{return owner.values.size;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("mutable-record-array", "define class Item as pub let int value=1; end pub fnc main():int{val Array<Item> a=new Array<Item>();return 0;}".to_owned(), foo.to_owned()),
        ("record-array-integer-forgery", "define class Item as pub val int value; end pub fnc main():int{val Array<Item> a=new Array<Item>();a.add(1);return 0;}".to_owned(), foo.to_owned()),
        ("record-array-wrong-nominal-type", "define class A as pub val int value; end define class B as pub val int value; end pub fnc main():int{val Array<A> a=new Array<A>();a.add(new B(1));return 0;}".to_owned(), foo.to_owned()),
        ("record-array-handle-return", "define class Item as pub val int value; end pub fnc main():int{val Array<Item> a=new Array<Item>();a.add(new Item(1));return a.get(0);}".to_owned(), foo.to_owned()),
        ("record-view-move", "define class Item as pub val int value; end pub fnc main():int{val Array<Item> a=new Array<Item>();a.add(new Item(1));val Item item=a.get(0);let mut Item moved=rt take item;return 0;}".to_owned(), foo.to_owned()),
        ("record-view-mutable-loan", "define class Item as pub val int value; end fnc edit(borrow mut Item item):int{return item.value;} pub fnc main():int{val Array<Item> a=new Array<Item>();a.add(new Item(1));val Item item=a.get(0);return edit(rt borrow mut item);}".to_owned(), foo.to_owned()),
        ("record-view-projected-mutable-loan", "define class Item as pub val int value; end fnc edit(borrow mut int value):int{return value;} pub fnc main():int{val Array<Item> a=new Array<Item>();a.add(new Item(1));val Item item=a.get(0);return edit(rt borrow mut item.value);}".to_owned(), foo.to_owned()),
        ("record-runtime-symbol-collision", "pub fnc ores_record_region_begin():int{return 0;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("string-record-integer-argument", "define class Segment as pub val String value; end pub fnc main():int{val Segment s=new Segment(1);return 0;}".to_owned(), foo.to_owned()),
        ("integer-record-string-argument", "define class Segment as pub val int kind; end pub fnc main():int{val Segment s=new Segment(\"bad\");return 0;}".to_owned(), foo.to_owned()),
        ("string-record-missing-argument", "define class Segment as pub val String value; end pub fnc main():int{val Segment s=new Segment();return 0;}".to_owned(), foo.to_owned()),
        ("string-record-private-field", "define class Segment as val String value; end pub fnc main():int{val Segment s=new Segment(\"secret\");return s.value==\"secret\"?0:1;}".to_owned(), foo.to_owned()),
        ("string-record-final-field-write", "define class Segment as pub val String value; end pub fnc main():int{let mut Segment s=new Segment(\"first\");s.value=\"other\";return 0;}".to_owned(), foo.to_owned()),
        ("string-record-untyped-handle-return", "define class Segment as pub val String value; end pub fnc main():int{val Segment s=new Segment(\"first\");return s.value;}".to_owned(), foo.to_owned()),
        ("string-array-integer-add", "pub fnc main():int{val Array<String> a=new Array<String>();a.add(42);return 0;}".to_owned(), foo.to_owned()),
        ("integer-array-string-add", "pub fnc main():int{val Array<int> a=new Array<int>();a.add(\"bad\");return 0;}".to_owned(), foo.to_owned()),
        ("string-array-string-index", "pub fnc main():int{val Array<String> a=new Array<String>();return a.get(\"bad\")==\"x\"?0:1;}".to_owned(), foo.to_owned()),
        ("string-array-integer-set", "pub fnc main():int{val Array<String> a=new Array<String>();a.set(0,42);return 0;}".to_owned(), foo.to_owned()),
        ("string-array-handle-as-int", "pub fnc main():int{val Array<String> a=new Array<String>();return a.get(0);}".to_owned(), foo.to_owned()),
        ("string-array-wrong-constructor", "pub fnc main():int{val Array<String> a=new Array<int>();return 0;}".to_owned(), foo.to_owned()),
        ("string-array-copy-as-int", "pub fnc main():int{val Array<String> a=new Array<String>();val n:int=a;return n;}".to_owned(), foo.to_owned()),
        ("string-array-take-into-int-field", "define class Box as pub mut Array<int> values=new Array<int>(); end pub fnc main():int{let mut Box b=new Box();val Array<String> a=new Array<String>();b.values=rt take a;return 0;}".to_owned(), foo.to_owned()),
        ("unsupported-object-array", "pub fnc main():int{val Array<Foo> a=new Array<Foo>();return 0;}".to_owned(), foo.to_owned()),
        (
            "string-integer-equality",
            r#"pub fnc main():int{return "a" == 1 ? 0 : 1;}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "string-ordering",
            r#"pub fnc main():int{return "a" < "b" ? 0 : 1;}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "private-method",
            exact.replace("f.bar()", "f.foo()"),
            foo.to_owned(),
        ),
        (
            "private-class",
            exact.to_owned(),
            foo.replace("define pub class", "define class"),
        ),
        (
            "unknown-class",
            exact.replace("import class Foo", "import class Missing"),
            foo.to_owned(),
        ),
        (
            "wrong-local-type",
            r#"pub fnc main():int{val x:int="bad";return x;}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "wrong-return-type",
            r#"pub fnc main():int{return "bad";}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "wrong-console-type",
            "pub fnc main():int{stdio.print(42);return 0;}".to_owned(),
            foo.to_owned(),
        ),
        (
            "bad-concat",
            r#"pub fnc main():int{val string x="bad"+true;return 0;}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "forward-local-read",
            r#"pub fnc main():int{stdio.print(x);val string x="bad";return 0;}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "unpaired-surrogate",
            r#"pub fnc main():int{stdio.print("\uD800");return 0;}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "runtime-symbol-collision",
            "fnc ores_string_literal(int x,int y):int{return 0;}pub fnc main():int{return 0;}"
                .to_owned(),
            foo.to_owned(),
        ),
        (
            "actor-string-region-boundary",
            r#"define actor Worker as {let total:int=0;pub fnc on_message(int n):int{return n;}}
                pub fnc main():int{stdio.print("unsafe");const worker:ActorRef<Worker> =spawn Worker();worker.send(1);stop worker;return 0;}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "print-is-void",
            r#"pub fnc main():int{return stdio.print("not an int");}"#.to_owned(),
            foo.to_owned(),
        ),
    ];
    for (name, source, dependency) in &negatives {
        fs::write(scratch.0.join("main.ores"), source).unwrap();
        fs::write(scratch.0.join("main-1.ores"), dependency).unwrap();
        let rejected = run(Command::new(&compiler).arg(scratch.0.join("main.ores")));
        assert_eq!(
            rejected.status.code(),
            Some(1),
            "{name}: must fail before codegen"
        );
        assert!(rejected.stdout.is_empty(), "{name}: emitted partial IR");
        let expected_boundary = match *name {
            "owning-return-duplicate-array" => Some("live unique array"),
            "owning-return-borrowed-field" => Some("named lexical array"),
            "owning-return-deferred-source-reuse" => Some("borrow of moved class"),
            "owning-result-use-after-class-move" => Some("use-after-move"),
            "owning-result-by-value-call" => Some("by-value class parameter"),
            "record-return-integer-forgery" | "record-return-wrong-nominal" |
            "record-return-handle-as-int" => Some("return type"),
            "record-return-mutable-identity" => Some("immutable scalar/String fields"),
            "record-return-borrowed-array-owner" | "record-return-incoming-borrow" |
            "record-return-moved-local" => Some("record return requires"),
            "record-return-not-os-status" => Some("OS entrypoint requires"),
            "record-return-unknown-type" => Some("unknown record class"),
            _ => None,
        };
        if let Some(boundary) = expected_boundary {
            assert!(String::from_utf8_lossy(&rejected.stderr).contains(boundary),
                "{name}: wrong boundary: {}", String::from_utf8_lossy(&rejected.stderr));
        }
        if *name == "actor-string-region-boundary" {
            assert!(
                String::from_utf8_lossy(&rejected.stderr)
                    .contains("native strings currently require"),
                "{name}: wrong rejection: {}",
                String::from_utf8_lossy(&rejected.stderr)
            );
        }
        assert!(
            !String::from_utf8_lossy(&rejected.stderr)
                .to_lowercase()
                .contains("internal"),
            "{name}: internal compiler failure"
        );
    }
    fs::write(
        scratch.0.join("main.ores"),
        r#"pub fnc main():int{stdio.print("host only");return 0;}"#,
    )
    .unwrap();
    for target in ["--target=wasm32", "--target=thumbv7m-none-eabi"] {
        let rejected = run(Command::new(&compiler)
            .arg(target)
            .arg(scratch.0.join("main.ores")));
        assert_eq!(
            rejected.status.code(),
            Some(1),
            "{target}: unsupported string target must fail"
        );
        assert!(rejected.stdout.is_empty(), "{target}: emitted partial IR");
        let boundary = if target == "--target=wasm32" {
            "native strings currently require"
        } else {
            "cortex-m target rejects main, effects or non-scalar functions"
        };
        assert!(
            String::from_utf8_lossy(&rejected.stderr).contains(boundary),
            "{target}: wrong rejection: {}",
            String::from_utf8_lossy(&rejected.stderr)
        );
    }
    assert!(!sanitize || guest_address_checks, "missing guest ASan instrumentation proof");
    if guest_address_checks { println!("PASS generated guest IR includes ASan memory checks"); }
    println!("PASS class/string parity: {} common-source positives, {} native-only positives (five native executions each), {} no-IR rejections plus two target boundaries, Java={comparisons}, standalone={standalone_runs}, sanitized={sanitize}", positives.len(), native_only.len(), negatives.len());
}
