import hashlib,json,pathlib,subprocess,sys,tempfile
raw=pathlib.Path(__file__).with_name("mandatory-async-io.json").read_bytes()
assert hashlib.sha256(raw).hexdigest()=="e3b5caa3267683e7a1ec83f04d671333455746ceb87188dba6ec7ec1e75ec042"
cases=json.loads(raw)["cases"]
with tempfile.TemporaryDirectory() as directory:
 source=pathlib.Path(directory)/"main.ores"
 for case in cases:
  source.write_text(case["source"])
  for flags in [[],["--core-io=native-async-only"]]:
   result=subprocess.run([sys.argv[1],*flags,str(source)],capture_output=True,text=True,timeout=10)
   if case["expect"]=="accept":assert result.returncode==0 and result.stdout.startswith("; ModuleID"),(case["id"],result)
   else:assert result.returncode==1 and not result.stdout and "oreslang-llvmc:" in result.stderr,(case["id"],result)
print(f"{len(cases)} shared mandatory I/O cases verified under default and explicit invariant assertion")
