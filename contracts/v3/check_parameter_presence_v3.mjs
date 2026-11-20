// Staged pure v3 semantics. Does NOT parse, compile, or execute Oreslang guest code.
// Run: node contracts/v3/check_parameter_presence_v3.mjs
const classify = function classify(declaration, items) {
  const fmt=v => v.kind==="none"?"None":v.kind==="string"?JSON.stringify(v.value):String(v.value);
  const slots=new Map(), vals=[], origins=[], evaluated=[];
  declaration.forEach((p,i)=>{
    if(!p.name||slots.has(p.name))throw Error("DUPLICATE_FORMAL");
    if(!["int","bool","string"].includes(p.base))throw Error("UNKNOWN_BASE_TYPE");
    if(p.required && (p.omittable||Object.hasOwn(p,"default")))throw Error("REQUIRED_CONFLICT");
    if(p.omittable&&!p.option&&!Object.hasOwn(p,"default"))throw Error("MISSING_NONOPTION_DEFAULT");
    if(Object.hasOwn(p,"default")){
      const d=p.default;
      if(!d||!["int","bool","string","none"].includes(d.kind))throw Error("BAD_DEFAULT");
      if(d.kind==="none"&&!p.option)throw Error("NONE_FOR_NONOPTION");
      if(d.kind!=="none"&&d.kind!==p.base)throw Error("DEFAULT_TYPE");
      if(d.kind==="int"&&!Number.isSafeInteger(d.value))throw Error("INTEGER_RANGE");
      if(d.kind==="bool"&&typeof d.value!=="boolean")throw Error("DEFAULT_VALUE");
      if(d.kind==="string"&&typeof d.value!=="string")throw Error("DEFAULT_VALUE");
    }
    slots.set(p.name,i);
  });
  let positional=0,seenNamed=false;const bound=new Map();
  items.forEach((a,i)=>{
    const named=Object.hasOwn(a,"name");
    if(named)seenNamed=true;
    if(!named&&seenNamed)throw Error("LATE_POSITIONAL");
    const slot=named?slots.get(a.name):positional++;
    if(slot===undefined)throw Error("UNKNOWN_LABEL");
    if(slot>=declaration.length)throw Error("EXCESS_ARGS");
    if(bound.has(slot))throw Error("DUPLICATE_BINDING");
    const hasValue=Object.hasOwn(a,"value");
    if(hasValue===Boolean(a.omit))throw Error("MALFORMED_ITEM");
    if(a.omit&&!((declaration[slot].omittable||Object.hasOwn(declaration[slot],"default"))))throw Error("OMIT_REQUIRED");
    bound.set(slot,{...a,at:i});
    if(hasValue)evaluated.push(a.source ?? String(i));
  });
  for(let i=0;i<declaration.length;i++){
    const p=declaration[i],a=bound.get(i),present=a&&Object.hasOwn(a,"value");
    if(present){
      const v=a.value;
      if(v.kind==="option"){
        if(!p.option||v.nominal!==true)throw Error("WRONG_OPTION_TYPE");
        vals.push(v.value);
      } else {
        if(v.kind!==p.base)throw Error("ACTUAL_TYPE");
        vals.push(p.option?"Some("+fmt(v)+")":fmt(v));
      }
      origins.push(a.at);
      continue;
    }
    if(!p.omittable&&!Object.hasOwn(p,"default"))throw Error("MISSING_REQUIRED");
    if(Object.hasOwn(p,"default")){
      const d=p.default;
      vals.push(d.kind==="none"?"None":p.option?"Some("+fmt(d)+")":fmt(d));
    }else{
      if(!p.option)throw Error("MISSING_NONOPTION_DEFAULT");
      vals.push("None");
    }
    origins.push(a?a.at:-1);
  }
  return {values:vals,origins,evaluated};
};
const I = value=>({kind:"int",value});
const S = value=>({kind:"string",value});
const B = value=>({kind:"bool",value});
const N = ()=>({kind:"none"});
const O = value=>({kind:"option",value,nominal:true});
const examples = [
  {
    "name": "typed option defaults produce Some",
    "d": [
      {
        "name": "a",
        "base": "int",
        "option": true,
        "default": {
          "kind": "int",
          "value": 5
        }
      },
      {
        "name": "b",
        "base": "string",
        "option": true,
        "default": {
          "kind": "string",
          "value": "charlie"
        }
      }
    ],
    "a": [],
    "values": [
      "Some(5)",
      "Some(\"charlie\")"
    ],
    "eval": []
  },
  {
    "name": "plain defaults remain plain",
    "d": [
      {
        "name": "a",
        "base": "int",
        "omittable": true,
        "default": {
          "kind": "int",
          "value": 5
        }
      },
      {
        "name": "b",
        "base": "string",
        "omittable": true,
        "default": {
          "kind": "string",
          "value": "charlie"
        }
      }
    ],
    "a": [],
    "values": [
      "5",
      "\"charlie\""
    ],
    "eval": []
  },
  {
    "name": "Option type alone does not imply omission",
    "d": [
      {
        "name": "a",
        "base": "int",
        "option": true
      }
    ],
    "a": [],
    "error": "MISSING_REQUIRED"
  },
  {
    "name": "ordinary omittable without default rejected",
    "d": [
      {
        "name": "a",
        "base": "int",
        "omittable": true
      }
    ],
    "a": [],
    "error": "MISSING_NONOPTION_DEFAULT"
  },
  {
    "name": "option and name '?' combine",
    "d": [
      {
        "name": "a",
        "base": "int",
        "option": true,
        "omittable": true
      }
    ],
    "a": [],
    "values": [
      "None"
    ],
    "eval": []
  },
  {
    "name": "literal lifts only at Option formal boundary",
    "d": [
      {
        "name": "a",
        "base": "int",
        "option": true
      }
    ],
    "a": [
      {
        "value": {
          "kind": "int",
          "value": 4
        },
        "source": "4"
      }
    ],
    "values": [
      "Some(4)"
    ],
    "eval": [
      "4"
    ]
  },
  {
    "name": "literal not Option for plain formal",
    "d": [
      {
        "name": "a",
        "base": "int"
      }
    ],
    "a": [
      {
        "value": {
          "kind": "option",
          "value": "None",
          "nominal": true
        }
      }
    ],
    "error": "WRONG_OPTION_TYPE"
  },
  {
    "name": "explicit None suppresses declared default",
    "d": [
      {
        "name": "a",
        "base": "int",
        "option": true,
        "default": {
          "kind": "int",
          "value": 5
        }
      }
    ],
    "a": [
      {
        "value": {
          "kind": "option",
          "value": "None",
          "nominal": true
        },
        "source": "none"
      }
    ],
    "values": [
      "None"
    ],
    "eval": [
      "none"
    ]
  },
  {
    "name": "explicit omission triggers default",
    "d": [
      {
        "name": "a",
        "base": "int",
        "option": true,
        "default": {
          "kind": "int",
          "value": 5
        }
      }
    ],
    "a": [
      {
        "omit": true
      }
    ],
    "values": [
      "Some(5)"
    ],
    "eval": []
  },
  {
    "name": "explicit omission on plain default uses plain value",
    "d": [
      {
        "name": "a",
        "base": "int",
        "default": {
          "kind": "int",
          "value": 9
        }
      }
    ],
    "a": [
      {
        "omit": true
      }
    ],
    "values": [
      "9"
    ],
    "eval": []
  },
  {
    "name": "explicit omit on required Option is forbidden",
    "d": [
      {
        "name": "a",
        "base": "int",
        "option": true
      }
    ],
    "a": [
      {
        "omit": true
      }
    ],
    "error": "OMIT_REQUIRED"
  },
  {
    "name": "None default for option remains outer None",
    "d": [
      {
        "name": "a",
        "base": "int",
        "option": true,
        "default": {
          "kind": "none"
        }
      }
    ],
    "a": [],
    "values": [
      "None"
    ],
    "eval": []
  },
  {
    "name": "None default invalid on plain int",
    "d": [
      {
        "name": "a",
        "base": "int",
        "default": {
          "kind": "none"
        }
      }
    ],
    "a": [],
    "error": "NONE_FOR_NONOPTION"
  },
  {
    "name": "source-order named permutation",
    "d": [
      {
        "name": "a",
        "base": "int"
      },
      {
        "name": "b",
        "base": "int",
        "default": {
          "kind": "int",
          "value": 8
        }
      },
      {
        "name": "c",
        "base": "int"
      }
    ],
    "a": [
      {
        "name": "c",
        "value": {
          "kind": "int",
          "value": 3
        },
        "source": "third"
      },
      {
        "name": "a",
        "value": {
          "kind": "int",
          "value": 1
        },
        "source": "first"
      }
    ],
    "values": [
      "1",
      "8",
      "3"
    ],
    "eval": [
      "third",
      "first"
    ],
    "origins": [
      1,
      -1,
      0
    ]
  },
  {
    "name": "named explicit omit maps raw origin",
    "d": [
      {
        "name": "a",
        "base": "int",
        "default": {
          "kind": "int",
          "value": 5
        }
      },
      {
        "name": "b",
        "base": "int"
      }
    ],
    "a": [
      {
        "name": "b",
        "value": {
          "kind": "int",
          "value": 8
        },
        "source": "b"
      },
      {
        "name": "a",
        "omit": true
      }
    ],
    "values": [
      "5",
      "8"
    ],
    "eval": [
      "b"
    ],
    "origins": [
      1,
      0
    ]
  },
  {
    "name": "late positional is invalid",
    "d": [
      {
        "name": "a",
        "base": "int"
      },
      {
        "name": "b",
        "base": "int"
      }
    ],
    "a": [
      {
        "name": "b",
        "value": {
          "kind": "int",
          "value": 1
        }
      },
      {
        "value": {
          "kind": "int",
          "value": 2
        }
      }
    ],
    "error": "LATE_POSITIONAL"
  },
  {
    "name": "duplicate labeled bindings invalid",
    "d": [
      {
        "name": "a",
        "base": "int"
      }
    ],
    "a": [
      {
        "name": "a",
        "value": {
          "kind": "int",
          "value": 1
        }
      },
      {
        "name": "a",
        "value": {
          "kind": "int",
          "value": 2
        }
      }
    ],
    "error": "DUPLICATE_BINDING"
  },
  {
    "name": "positional + named collision",
    "d": [
      {
        "name": "a",
        "base": "int"
      }
    ],
    "a": [
      {
        "value": {
          "kind": "int",
          "value": 1
        }
      },
      {
        "name": "a",
        "value": {
          "kind": "int",
          "value": 2
        }
      }
    ],
    "error": "DUPLICATE_BINDING"
  },
  {
    "name": "unknown named argument rejected",
    "d": [
      {
        "name": "a",
        "base": "int"
      }
    ],
    "a": [
      {
        "name": "b",
        "value": {
          "kind": "int",
          "value": 4
        }
      }
    ],
    "error": "UNKNOWN_LABEL"
  },
  {
    "name": "unknown formal duplicate rejected",
    "d": [
      {
        "name": "a",
        "base": "int"
      },
      {
        "name": "a",
        "base": "int"
      }
    ],
    "a": [],
    "error": "DUPLICATE_FORMAL"
  },
  {
    "name": "missing required despite other optional",
    "d": [
      {
        "name": "a",
        "base": "int",
        "default": {
          "kind": "int",
          "value": 3
        }
      },
      {
        "name": "b",
        "base": "bool"
      }
    ],
    "a": [],
    "error": "MISSING_REQUIRED"
  },
  {
    "name": "wrong annotated default type",
    "d": [
      {
        "name": "a",
        "base": "int",
        "default": {
          "kind": "string",
          "value": "oops"
        }
      }
    ],
    "a": [],
    "error": "DEFAULT_TYPE"
  },
  {
    "name": "required-default conflict",
    "d": [
      {
        "name": "a",
        "base": "int",
        "option": true,
        "required": true,
        "default": {
          "kind": "int",
          "value": 5
        }
      }
    ],
    "a": [],
    "error": "REQUIRED_CONFLICT"
  },
  {
    "name": "forged Option value rejected",
    "d": [
      {
        "name": "a",
        "base": "int",
        "option": true
      }
    ],
    "a": [
      {
        "value": {
          "kind": "option",
          "value": "None",
          "nominal": false
        }
      }
    ],
    "error": "WRONG_OPTION_TYPE"
  },
  {
    "name": "int default must be representable",
    "d": [
      {
        "name": "a",
        "base": "int",
        "default": {
          "kind": "int",
          "value": 1e+22
        }
      }
    ],
    "a": [],
    "error": "INTEGER_RANGE"
  },
  {
    "name": "non-option explicit None rejected",
    "d": [
      {
        "name": "a",
        "base": "int",
        "default": {
          "kind": "int",
          "value": 5
        }
      }
    ],
    "a": [
      {
        "value": {
          "kind": "option",
          "value": "None",
          "nominal": true
        }
      }
    ],
    "error": "WRONG_OPTION_TYPE"
  },
  {
    "name": "extra positional values rejected",
    "d": [
      {
        "name": "a",
        "base": "int"
      }
    ],
    "a": [
      {
        "value": {
          "kind": "int",
          "value": 2
        }
      },
      {
        "value": {
          "kind": "int",
          "value": 3
        }
      }
    ],
    "error": "EXCESS_ARGS"
  },
  {
    "name": "malformed omit and value in one item rejected",
    "d": [
      {
        "name": "a",
        "base": "int",
        "default": {
          "kind": "int",
          "value": 1
        }
      }
    ],
    "a": [
      {
        "omit": true,
        "value": {
          "kind": "int",
          "value": 2
        }
      }
    ],
    "error": "MALFORMED_ITEM"
  },
  {
    "name": "option value not implicitly nested-lifted",
    "d": [
      {
        "name": "a",
        "base": "int",
        "option": true
      }
    ],
    "a": [
      {
        "value": {
          "kind": "option",
          "value": "Some(None)",
          "nominal": true
        },
        "source": "option"
      }
    ],
    "values": [
      "Some(None)"
    ],
    "eval": [
      "option"
    ]
  },
  {
    "name": "bool defaults are typed and plain",
    "d": [
      {
        "name": "a",
        "base": "bool",
        "default": {
          "kind": "bool",
          "value": false
        }
      }
    ],
    "a": [],
    "values": [
      "false"
    ],
    "eval": []
  }
];
const runTest = function runTest(f){
  let actual;
  try{actual=classify(f.d,f.a);}catch(e){if(e.message===f.error)return;throw Error(f.name+" expected "+f.error+" got "+e.message)}
  if(f.error)throw Error(f.name+" expected "+f.error+" but succeeded");
  for(const [key,wanted] of [["values",f.values],["evaluated",f.eval],["origins",f.origins]]){
    if(wanted!==undefined&&JSON.stringify(actual[key])!==JSON.stringify(wanted))throw Error(f.name+" "+key+" mismatch: "+JSON.stringify(actual[key]));
  }
};
for (const f of examples) runTest(f);
let seed = 0xC0FFEE;
function rand(n){seed=(Math.imul(seed,1664525)+1013904223)>>>0;return seed%n}
for(let trial=0;trial<5000;trial++){
 const numbers=[1,2,3,4].map(x=>x+rand(100));
 const permutation=[0,1,2,3];
 for(let k=3;k>0;k--){const j=rand(k+1);[permutation[j],permutation[k]]=[permutation[k],permutation[j]]}
 const d=[0,1,2,3].map(i=>({name:"p"+i,base:"int",default:I(-1)}));
 const a=permutation.map(i=>({name:"p"+i,value:I(numbers[i]),source:"p"+i}));
 const v=classify(d,a);
 if(JSON.stringify(v.values)!==JSON.stringify(numbers.map(String))||JSON.stringify(v.evaluated)!==JSON.stringify(permutation.map(i=>"p"+i)))throw Error("random permutation lost values/order at "+trial);
}
console.log("PASS: "+examples.length+" deterministic v3 semantic vectors + 5000 named-order property trials");
