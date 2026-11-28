#!/usr/bin/env node
// Exact source-file compiled-guest smoke/trap observations for core-four-v1.
// FAIL CLOSED: no compiler, no artifact, no trap witness => nonzero exit.
// A successful local observation is NOT independent CI/source provenance proof.
import {readFileSync, existsSync, statSync, mkdtempSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';

const root = resolve(fileURLToPath(new URL('..', import.meta.url)));
const SHA = b => createHash('sha256').update(b).digest('hex');
const results=[];
let failures=0;
const lane=process.env.ATTO_CORE_FOUR_LANE || null;
const suite=JSON.parse(readFileSync(join(root,'contracts/core-four-v1.json'),'utf8'));
const extension=process.env.ATTO_CORE_FOUR_ARTIFACT_EXT || '.bin';
function argvFrom(key, placeholders) {
  const raw=process.env[key];
  if (!raw) throw Error(key+' required; no simulator or silent skip');
  const cmd=JSON.parse(raw);
  if(!Array.isArray(cmd) || !cmd.length ||
     !cmd.every(x=>typeof x==='string' && x.length && !x.includes('\n')))
    throw Error(key+' must be a nonempty argv string array');
  for(const token of placeholders)
    if(!cmd.some(x=>x.includes(token))) throw Error(key+' must contain '+token);
  return cmd;
}
function launch(args, values) {
  const v=args.map(s=>s.replace(/\{(source|artifact)\}/g, (_,k)=>values[k]));
  const p=spawnSync(v[0], v.slice(1), {
    cwd:root, shell:false, encoding:'utf8',
    timeout:120000, maxBuffer:1024*1024,
    env:{...process.env, ATTO_CORE_FOUR_TEST_CASE:values.name}
  });
  if(p.error) throw Error('spawn error: '+p.error.message);
  if(p.signal || !Number.isInteger(p.status)) throw Error('compiler/guest terminated by signal or timeout');
  return {exit:p.status,stdout:p.stdout||'',stderr:p.stderr||''};
}
const exec=(a,v)=>launch(a,v);
let work=null;
try {
  if(!/^[a-z0-9][a-z0-9-]{1,50}$/.test(lane||''))
    throw Error('ATTO_CORE_FOUR_LANE required');
  if(!/^\.[A-Za-z0-9]{1,8}$/.test(extension))
    throw Error('artifact extension must be a short ASCII suffix');
  if(suite.schema!=='attolang.core-four.v1') throw Error('suite mismatch');
  const compile=argvFrom('ATTO_CORE_FOUR_COMPILE_ARGV_JSON',['{source}','{artifact}']);
  const run=argvFrom('ATTO_CORE_FOUR_RUN_ARGV_JSON',['{artifact}']);
  // Only checked-in fixtures, NEVER a generated source or host oracle.
  work=mkdtempSync(join(tmpdir(),'atto-core-four-'));
  for(const [name,file,expectedExit] of [
    ['smoke','smoke.atto',42],
    ['contract_failure','contract-failure.atto',null]
  ]) {
    const source=join(root,'tests/fixtures/core-four-v1',file);
    const artifact=join(work,name+extension);
    const srcHash=SHA(readFileSync(source));
    try {
      if(existsSync(artifact)) throw Error('non-fresh artifact');
      const params={source,artifact,name};
      const compiled=exec(compile,params);
      if(compiled.exit!==0) throw Error('real compiler rejected '+name+': '+compiled.stderr.slice(0,500));
      if(SHA(readFileSync(source))!==srcHash) throw Error('compiler changed checked-in fixture');
      if(!existsSync(artifact)||!statSync(artifact).isFile()||statSync(artifact).size===0)
        throw Error('compiler produced no real artifact');
      const artifactHash=SHA(readFileSync(artifact));
      const executed=exec(run,params);
      if(SHA(readFileSync(source))!==srcHash || SHA(readFileSync(artifact))!==artifactHash)
        throw Error('guest mutated source or artifact');
      if(expectedExit!==null) {
        if(executed.exit!==expectedExit)
          throw Error('smoke exit '+executed.exit+' != '+expectedExit+': '+executed.stderr.slice(0,500));
      } else {
        if(executed.exit===0 || executed.exit===42)
          throw Error('contracts.requires(false) was optimized away or returned');
        const output=executed.stderr+'\n'+executed.stdout;
        if(!output.includes('core-four-v1: precondition failed'))
          throw Error('negative guest failed without required contract error witness');
      }
      results.push({name,source_sha256:srcHash,artifact_sha256:artifactHash,
        compile_exit:compiled.exit,guest_exit:executed.exit,
        guest_stdout_sha256:SHA(executed.stdout),guest_stderr_sha256:SHA(executed.stderr),
        status:'local-executed-observation'});
    }catch(e) {
      failures++; results.push({name,source_sha256:srcHash,status:'failed',detail:String(e.message)});
    }
  }
}catch(e) {failures++;results.push({name:'configuration',status:'failed',detail:String(e.message)});}
finally {if(work) rmSync(work,{recursive:true,force:true});}
const git=spawnSync('git',['rev-parse','HEAD'],{cwd:root,shell:false,encoding:'utf8'});
const head=git.status===0&&/^[0-9a-f]{40}$/.test(git.stdout.trim())?git.stdout.trim():null;
const report={schema:'attolang.core-four.local-observation.v1',
  evidence_class:'NON_CERTIFYING_LOCAL_COMPILER_OBSERVATIONS',
  lane,repository_head_sha:head,passed:results.filter(x=>x.status==='local-executed-observation').length,
  failed:failures,certified_guest_passes:0,observations:results,
  warning:'Do not promote to CI certified without exact-head source, runner steps, compiler/guest provenance and independent checks'};
console.log(JSON.stringify(report,null,2));
if(failures || results.length!==2) process.exitCode=1;
