//! Compiler-owned physical layout planning. Explicit target storage facts,
//! distinct nominal IDs, declaration-order fields, no implicit boxing.
//! Not an FFI calling convention or an ownership/placement authorization.
use std::collections::BTreeSet;
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub(crate) struct Storage { pub size:u64, pub align:u64 }
#[derive(Clone,Debug,PartialEq,Eq)]
pub(crate) enum FieldType {
    Scalar(Storage), Inline(usize),
    // Typed planner primitives; source admission is a separate change.
    #[allow(dead_code)]
    ScalarArray(Storage,u64),
    #[allow(dead_code)]
    InlineArray(usize,u64),
}
#[derive(Clone,Debug)]
pub(crate) struct Owner { pub identity:String, pub fields:Vec<(String,FieldType)> }
#[derive(Clone,Debug,PartialEq,Eq)]
pub(crate) struct Field { pub name:String,pub offset:u64,pub storage:Storage,pub kind:FieldType }
#[derive(Clone,Debug,PartialEq,Eq)]
pub(crate) struct Layout { pub identity:String,pub storage:Storage,pub fields:Vec<Field> }
#[derive(Clone,Copy,Debug)]
pub(crate) struct Limits { pub max_size:u64,pub max_align:u64,pub max_owners:usize,pub max_fields:usize }
fn err(code:&str)->String{format!("E_LAYOUT_{code}")}
fn valid(s:Storage,l:Limits)->Result<(),String>{
    if s.size==0||!s.align.is_power_of_two()||s.align>l.max_align||s.size%s.align!=0{return Err(err("INVALID_STORAGE"));}
    if s.size>l.max_size{return Err(err("LIMIT"));}Ok(())
}
fn round(v:u64,a:u64)->Result<u64,String>{
    v.checked_add(a-1).map(|n|n&!(a-1)).ok_or_else(||err("OVERFLOW"))
}
fn array_storage(element:Storage,count:u64,limits:Limits)->Result<Storage,String>{
    valid(element,limits)?;
    if count==0{return Err(err("ARRAY_COUNT"));}
    let size=element.size.checked_mul(count).ok_or_else(||err("OVERFLOW"))?;
    let storage=Storage{size,align:element.align};
    valid(storage,limits)?;Ok(storage)
}
/// Resolve a finite graph in dependency order. Forward owners are allowed.
/// Bounded iterative traversal avoids stack overflow even for cyclic input.
/// All sizes and alignments are explicit inputs; no implicit host ABI facts.
pub(crate) fn plan(owners:&[Owner],limits:Limits)->Result<Vec<Layout>,String>{
    if owners.is_empty()||owners.len()>limits.max_owners.min(256)||limits.max_size==0||!limits.max_align.is_power_of_two(){return Err(err("LIMIT"));}
    let mut ids=BTreeSet::new();let mut pending=vec![0usize;owners.len()];
    let mut parents=vec![Vec::<usize>::new();owners.len()];let mut total=0usize;
    for (i,o) in owners.iter().enumerate(){
        if o.identity.is_empty()||o.identity.len()>1024||!ids.insert(&o.identity){return Err(err("IDENTITY"));}
        if o.fields.is_empty(){return Err(err("EMPTY"));}
        total=total.checked_add(o.fields.len()).ok_or_else(||err("LIMIT"))?;
        if total>limits.max_fields.min(4096){return Err(err("LIMIT"));}
        let mut names=BTreeSet::new();
        for (name,kind) in &o.fields{
            if name.is_empty()||name.len()>256||!names.insert(name){return Err(err("FIELD"));}
            match kind{
                FieldType::Scalar(s)=>valid(*s,limits)?,
                FieldType::ScalarArray(s,n)=>{array_storage(*s,*n,limits)?;}
                FieldType::Inline(j)|FieldType::InlineArray(j,_)=>{
                    if *j>=owners.len(){return Err(err("UNRESOLVED"));}
                    if matches!(kind,FieldType::InlineArray(_,0)){return Err(err("ARRAY_COUNT"));}
                    pending[i]+=1;parents[*j].push(i);
                }
            }
        }
    }
    let mut ready:BTreeSet<usize>=pending.iter().enumerate().filter_map(|(i,n)|(*n==0).then_some(i)).collect();
    let mut layouts=vec![None::<Layout>;owners.len()];
    while let Some(i)=ready.pop_first(){
        let mut size=0u64;let mut alignment=1u64;let mut fields=Vec::new();
        for (name,kind) in &owners[i].fields{
            let storage=match kind{
                FieldType::Scalar(s)=>*s,
                FieldType::Inline(j)=>layouts[*j].as_ref().ok_or_else(||err("UNRESOLVED"))?.storage,
                FieldType::ScalarArray(s,n)=>array_storage(*s,*n,limits)?,
                FieldType::InlineArray(j,n)=>array_storage(layouts[*j].as_ref().ok_or_else(||err("UNRESOLVED"))?.storage,*n,limits)?,
            };
            let offset=round(size,storage.align)?;
            size=offset.checked_add(storage.size).ok_or_else(||err("OVERFLOW"))?;
            if size>limits.max_size{return Err(err("LIMIT"));}
            alignment=alignment.max(storage.align);
            fields.push(Field{name:name.clone(),offset,storage,kind:kind.clone()});
        }
        let storage=Storage{size:round(size,alignment)?,align:alignment};valid(storage,limits)?;
        layouts[i]=Some(Layout{identity:owners[i].identity.clone(),storage,fields});
        for &parent in &parents[i]{pending[parent]-=1;if pending[parent]==0{ready.insert(parent);}}
    }
    layouts.into_iter().map(|v|v.ok_or_else(||err("CYCLE"))).collect()
}
impl Layout{
    /// Field indices select nominal fields; after an array field the next
    /// index selects an element. A path ending at an array selects its full
    /// storage. This is layout only, never authorization for a loan.
    pub(crate) fn project(&self,layouts:&[Layout],path:&[usize])->Result<(u64,Storage),String>{
        if path.is_empty()||path.len()>256{return Err(err("PROJECTION"));}
        let mut owner=self;let mut offset=0u64;let mut indices=path.iter().copied().peekable();
        let finish=|offset:u64,storage:Storage|{
            if offset.checked_add(storage.size).ok_or_else(||err("OVERFLOW"))?>self.storage.size{return Err(err("PROJECTION"));}
            Ok((offset,storage))
        };
        while let Some(index)=indices.next(){
            let field=owner.fields.get(index).ok_or_else(||err("PROJECTION"))?;
            offset=offset.checked_add(field.offset).ok_or_else(||err("OVERFLOW"))?;
            if indices.peek().is_none(){return finish(offset,field.storage);}
            let (element,count,next)=match field.kind{
                FieldType::Scalar(_)=>return Err(err("PROJECTION")),
                FieldType::Inline(j)=>{owner=layouts.get(j).ok_or_else(||err("PROJECTION"))?;continue;}
                FieldType::ScalarArray(s,n)=>(s,n,None),
                FieldType::InlineArray(j,n)=>{
                    let layout=layouts.get(j).ok_or_else(||err("PROJECTION"))?;
                    (layout.storage,n,Some(layout))
                }
            };
            let cell=u64::try_from(indices.next().ok_or_else(||err("PROJECTION"))?).map_err(|_|err("PROJECTION"))?;
            if cell>=count||element.size==0||!element.align.is_power_of_two(){return Err(err("PROJECTION"));}
            let delta=element.size.checked_mul(cell).ok_or_else(||err("OVERFLOW"))?;
            if delta.checked_add(element.size).ok_or_else(||err("OVERFLOW"))?>field.storage.size{return Err(err("PROJECTION"));}
            offset=offset.checked_add(delta).ok_or_else(||err("OVERFLOW"))?;
            if indices.peek().is_none(){return finish(offset,element);}
            owner=next.ok_or_else(||err("PROJECTION"))?;
        }
        Err(err("PROJECTION"))
    }
}
#[cfg(test)]
pub(crate) mod tests{
    use super::*;
    pub(crate) fn mixed()->(Vec<Owner>,Limits){
        let s=|size,align|FieldType::Scalar(Storage{size,align});
        (vec![
            Owner{identity:"m.Packet".into(),fields:vec![("tag".into(),s(1,1)),("pair".into(),FieldType::Inline(1)),("tail".into(),s(2,2))]},
            Owner{identity:"m.Pair".into(),fields:vec![("x".into(),s(8,8)),("flag".into(),s(1,1))]},
            Owner{identity:"n.Pair".into(),fields:vec![("x".into(),s(8,8)),("flag".into(),s(1,1))]},
        ],Limits{max_size:65536,max_align:8,max_owners:16,max_fields:64})
    }
    #[test]fn forward_nested_alignment_padding_and_nominal_identity(){
        let (owners,limits)=mixed();let p=plan(&owners,limits).unwrap();
        assert_eq!(p[0].storage,Storage{size:32,align:8});
        assert_eq!(p[1].storage,Storage{size:16,align:8});
        assert_eq!(p[0].fields.iter().map(|f|f.offset).collect::<Vec<_>>(),vec![0,8,24]);
        assert_eq!(p[0].project(&p,&[1,1]).unwrap(),(16,Storage{size:1,align:1}));
        assert_eq!(p[0].project(&p,&[1,0]).unwrap(),(8,Storage{size:8,align:8}));
        assert_eq!(p[1].fields,p[2].fields);assert_ne!(p[1].identity,p[2].identity);
        assert_eq!(p,plan(&owners,limits).unwrap());
        assert!(p[0].project(&p,&[0,0]).is_err());assert!(p[0].project(&p,&[1,2]).is_err());
    }
    #[test]fn cycles_missing_types_invalid_alignment_and_budget_fail_closed(){
        let (owners,limits)=mixed();
        for (kind,expected) in [(FieldType::Inline(0),"CYCLE"),(FieldType::Inline(99),"UNRESOLVED"),(FieldType::Scalar(Storage{size:3,align:3}),"INVALID_STORAGE"),(FieldType::Scalar(Storage{size:0,align:1}),"INVALID_STORAGE")]{
            let mut o=owners.clone();o[0].fields[0].1=kind;assert_eq!(plan(&o,limits).unwrap_err(),err(expected));
        }
        let mut o=owners.clone();o[1].fields[0].1=FieldType::Inline(0);assert_eq!(plan(&o,limits).unwrap_err(),err("CYCLE"));
        let mut o=owners.clone();o[2].identity=o[1].identity.clone();assert_eq!(plan(&o,limits).unwrap_err(),err("IDENTITY"));
        let mut o=owners.clone();o[0].fields[1].0=o[0].fields[0].0.clone();assert_eq!(plan(&o,limits).unwrap_err(),err("FIELD"));
        assert_eq!(plan(&owners,Limits{max_size:31,..limits}).unwrap_err(),err("LIMIT"));
        assert_eq!(plan(&owners,Limits{max_fields:6,..limits}).unwrap_err(),err("LIMIT"));
        assert_eq!(plan(&owners,Limits{max_owners:2,..limits}).unwrap_err(),err("LIMIT"));
    }
    #[test]fn fixed_array_stride_padding_and_nested_projection(){
        let (mut owners,limits)=mixed();
        owners[0].fields[1].1=FieldType::InlineArray(1,3);
        owners[0].fields[2].1=FieldType::ScalarArray(Storage{size:2,align:2},3);
        let p=plan(&owners,limits).unwrap();
        assert_eq!(p[0].storage,Storage{size:64,align:8});
        assert_eq!(p[0].fields.iter().map(|f|f.offset).collect::<Vec<_>>(),vec![0,8,56]);
        assert_eq!(p[0].project(&p,&[1]).unwrap(),(8,Storage{size:48,align:8}));
        assert_eq!(p[0].project(&p,&[1,2]).unwrap(),(40,Storage{size:16,align:8}));
        assert_eq!(p[0].project(&p,&[1,2,1]).unwrap(),(48,Storage{size:1,align:1}));
        assert_eq!(p[0].project(&p,&[2,2]).unwrap(),(60,Storage{size:2,align:2}));
        for path in [&[1,3][..],&[1,2,2],&[2,3],&[2,0,0],&[0,0],&[1,usize::MAX]]{
            assert!(p[0].project(&p,path).is_err(),"{path:?}");
        }
        assert_eq!(plan(&owners,Limits{max_size:63,..limits}).unwrap_err(),err("LIMIT"));
        assert_eq!(p,plan(&owners,limits).unwrap());
    }
    #[test]fn fixed_array_counts_cycles_and_overflow_fail_closed(){
        let (owners,limits)=mixed();
        for (kind,expected) in [
            (FieldType::ScalarArray(Storage{size:2,align:2},0),"ARRAY_COUNT"),
            (FieldType::InlineArray(1,0),"ARRAY_COUNT"),
            (FieldType::InlineArray(0,1),"CYCLE"),
            (FieldType::InlineArray(99,1),"UNRESOLVED"),
            (FieldType::ScalarArray(Storage{size:2,align:2},u64::MAX),"OVERFLOW"),
            (FieldType::InlineArray(1,u64::MAX),"OVERFLOW"),
            (FieldType::ScalarArray(Storage{size:2,align:2},32769),"LIMIT"),
            (FieldType::ScalarArray(Storage{size:3,align:2},1),"INVALID_STORAGE"),
        ]{
            let mut o=owners.clone();o[0].fields[1].1=kind;
            assert_eq!(plan(&o,limits).unwrap_err(),err(expected));
        }
    }
    #[test]fn checked_overflow_and_deep_graph(){
        let limits=Limits{max_size:u64::MAX,max_align:8,max_owners:256,max_fields:4096};
        let o=[Owner{identity:"Big".into(),fields:vec![("a".into(),FieldType::Scalar(Storage{size:u64::MAX,align:1})),("b".into(),FieldType::Scalar(Storage{size:8,align:8}))]}];
        assert_eq!(plan(&o,limits).unwrap_err(),err("OVERFLOW"));
        let mut chain=Vec::new();
        for i in 0..256{chain.push(Owner{identity:format!("T{i}"),fields:vec![("a".into(),if i==255{FieldType::Scalar(Storage{size:8,align:8})}else{FieldType::Inline(i+1)})]});}
        let p=plan(&chain,limits).unwrap();assert_eq!(p[0].storage.size,8);
        chain[255].fields[0].1=FieldType::Inline(0);assert_eq!(plan(&chain,limits).unwrap_err(),err("CYCLE"));
    }
}
