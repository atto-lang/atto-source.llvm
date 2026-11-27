import test from 'node:test';
import assert from 'node:assert/strict';
import {validRejection} from './copy_guest_receipt.mjs';
const input='/tmp/guest.atto', code='E_COPY_READONLY';
const good={status:1,signal:null,stderr:input+':7:5: '+code+': shared descendant\n'};
test('exact input and diagnostic code required',()=>{
  assert.equal(validRejection(input,code,good,false),true);
  for(const stderr of [input+':7:5: E_COPY_READONLY_SLOT: wrong code',input+':7:5: E_TYPE: mentions E_COPY_READONLY',input+'.bak:7:5: E_COPY_READONLY: wrong file',good.stderr+'extra diagnostic\n',input+':7:5: E_COPY_READONLY']) {
    assert.equal(validRejection(input,code,{...good,stderr},false),false);
  }
});
test('compiler crash, usage failure and artifact never count as rejection',()=>{
  for(const run of [{...good,status:0},{...good,status:2},{...good,status:101},{...good,status:null,signal:'SIGKILL'},{...good,error:new Error('timeout')},{...good,stderr:null}]) {
    assert.equal(validRejection(input,code,run,false),false);
  }
  assert.equal(validRejection(input,code,good,true),false);
});
test('diagnostic coordinates and artifact absence must be explicit',()=>{
  for(const location of ['0:5','7:0','07:5','7:05','9007199254740992:5','7:9007199254740992']) {
    assert.equal(validRejection(input,code,{...good,stderr:input+':'+location+': '+code+': shared descendant'},false),false,location);
  }
  for(const artifact of [undefined,null,0,'false',{}]) assert.equal(validRejection(input,code,good,artifact),false);
  for(const run of [null,undefined,{...good,signal:undefined},{...good,status:'1'}]) assert.equal(validRejection(input,code,run,false),false);
  assert.equal(validRejection(input,[code],good,false),false);
});
test('one nonempty diagnostic and one optional terminator',()=>{
  for(const ending of ['', '\n', '\r\n']) {
    assert.equal(validRejection(input,code,{...good,stderr:good.stderr.trimEnd()+ending},false),true);
  }
  for(const stderr of [good.stderr+'\n',good.stderr+'\r\n',good.stderr+' ',input+':7:5: '+code+':   ',input+':7:5: '+code+': shared\rdescendant']) {
    assert.equal(validRejection(input,code,{...good,stderr},false),false);
  }
});
