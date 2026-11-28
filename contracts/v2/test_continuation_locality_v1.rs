//! Host-only continuation placement reference model; not guest execution evidence.
#[derive(Clone, Copy)]
struct Worker { slot: i32, generation: u64, domain: &'static str, pending: i32, accepting: bool, tenant: u64 }
#[derive(Clone, Copy)]
struct Resume { domain: &'static str, actor_generation: u64, preferred_slot: i32, preferred_generation: u64, tenant: u64, cancelled: bool, expired: bool, code_current: bool, physical_active: bool }

fn choose(r: Resume, actor_generation: u64, workers: &[Worker], slack: i32) -> Result<Option<i32>, &'static str> {
    if r.domain.is_empty() || r.tenant == 0 || actor_generation == 0 || r.actor_generation == 0 { return Err("invalid actor"); }
    if !(0..=1024).contains(&slack) { return Err("invalid slack"); }
    if workers.len() > 256 || workers.iter().enumerate().any(|(i,w)| w.slot < 0 || w.generation == 0 || w.domain.is_empty() || w.pending < 0 || w.tenant == 0 || workers[..i].iter().any(|v| v.slot == w.slot)) { return Err("invalid workers"); }
    if r.cancelled || r.expired || !r.code_current || r.physical_active || r.actor_generation != actor_generation { return Ok(None); }
    let eligible = || workers.iter().filter(|w| w.accepting && w.domain == r.domain && w.tenant == r.tenant);
    let Some(least) = eligible().min_by_key(|w| (w.pending, w.slot)) else { return Ok(None); };
    if let Some(w) = eligible().find(|w| w.slot == r.preferred_slot && w.generation == r.preferred_generation && i64::from(w.pending) <= i64::from(least.pending) + i64::from(slack)) { return Ok(Some(w.slot)); }
    Ok(Some(least.slot))
}
fn fixture() -> (Resume, [Worker;4]) {
    let worker = Worker { slot: 0, generation: 4, domain: "SHARED", pending: 0, accepting: true, tenant: 1 };
    (Resume { domain: "SHARED", actor_generation: 11, preferred_slot: 0, preferred_generation: 4, tenant: 1, cancelled: false, expired: false, code_current: true, physical_active: false },
     [worker, Worker {slot:1,generation:8,..worker}, Worker {slot:2,generation:3,domain:"CONTROL",..worker}, Worker {slot:3,generation:2,domain:"UNTRUSTED",..worker}])
}
#[test] fn preferred_idle() { let (r,w)=fixture(); assert_eq!(choose(r,11,&w,1),Ok(Some(0))); }
#[test] fn small_queue() { let (r,mut w)=fixture(); w[0].pending=1; assert_eq!(choose(r,11,&w,1),Ok(Some(0))); }
#[test] fn backlog_spill() { let (r,mut w)=fixture(); w[0].pending=3; assert_eq!(choose(r,11,&w,1),Ok(Some(1))); }
#[test] fn zero_slack() { let (r,mut w)=fixture(); w[0].pending=1; assert_eq!(choose(r,11,&w,0),Ok(Some(1))); }
#[test] fn stale_hint() { let (mut r,mut w)=fixture(); r.preferred_generation=3; w[0].pending=2; assert_eq!(choose(r,11,&w,1),Ok(Some(1))); }
#[test] fn retired_worker() { let (r,mut w)=fixture(); w[0].accepting=false; assert_eq!(choose(r,11,&w,1),Ok(Some(1))); }
#[test] fn foreign_tenant_worker() { let (r,mut w)=fixture(); w[0].tenant=2; assert_eq!(choose(r,11,&w,1),Ok(Some(1))); }
#[test] fn foreign_tenant_only() { let (mut r,w)=fixture(); r.tenant=2; assert_eq!(choose(r,11,&w,1),Ok(None)); }
#[test] fn invalid_tenant() { let (mut r,mut w)=fixture(); r.tenant=0; assert!(choose(r,11,&w,1).is_err()); r.tenant=1; w[0].tenant=0; assert!(choose(r,11,&w,1).is_err()); }
#[test] fn foreign_preferred_worker() { let (mut r,w)=fixture(); r.preferred_slot=2; r.preferred_generation=3; assert_eq!(choose(r,11,&w,1),Ok(Some(0))); }
#[test] fn foreign_workers_only() { let (r,w)=fixture(); assert_eq!(choose(r,11,&w[2..],1),Ok(None)); }
#[test] fn no_inline_fallback() { let (r,_)=fixture(); assert_eq!(choose(r,11,&[],1),Ok(None)); }
#[test] fn cancelled() { let (mut r,w)=fixture(); r.cancelled=true; assert_eq!(choose(r,11,&w,1),Ok(None)); }
#[test] fn expired() { let (mut r,w)=fixture(); r.expired=true; assert_eq!(choose(r,11,&w,1),Ok(None)); }
#[test] fn stale_actor() { let (r,w)=fixture(); assert_eq!(choose(r,12,&w,1),Ok(None)); }
#[test] fn stale_code() { let (mut r,w)=fixture(); r.code_current=false; assert_eq!(choose(r,11,&w,1),Ok(None)); }
#[test] fn active_physical_turn() { let (mut r,w)=fixture(); r.physical_active=true; assert_eq!(choose(r,11,&w,1),Ok(None)); }
#[test] fn control_domain() { let (mut r,w)=fixture(); r.domain="CONTROL"; assert_eq!(choose(r,11,&w,1),Ok(Some(2))); }
#[test] fn no_hint() { let (mut r,w)=fixture(); r.preferred_slot=-1; assert_eq!(choose(r,11,&w,1),Ok(Some(0))); }
#[test] fn malformed_snapshot() {
    let (r,w)=fixture();
    for v in [Worker {slot:-1,..w[0]},Worker {generation:0,..w[0]},Worker {domain:"",..w[0]},Worker {pending:-1,..w[0]}] { assert!(choose(r,11,&[v],1).is_err()); }
    assert!(choose(r,11,&[w[0],Worker {generation:2,..w[0]}],1).is_err());
    let too_many:Vec<_>=(0..257).map(|slot| Worker {slot,..w[0]}).collect();
    assert!(choose(r,11,&too_many,1).is_err());
    for slack in [-1,1025] { assert!(choose(r,11,&w,slack).is_err()); }
    for bad in [Resume {domain:"",..r},Resume {actor_generation:0,..r}] { assert!(choose(bad,11,&w,1).is_err()); }
    assert!(choose(r,0,&w,1).is_err());
}
#[test] fn deterministic_property_sweep() {
    let (r,w)=fixture(); let mut state=20261101_u64;
    let mut next=||{state=state.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);state>>32};
    for _ in 0..2000 {
        let mut workers=w; for worker in &mut workers { worker.pending=(next()%12) as i32; worker.accepting=next()%2==1; }
        let request=Resume {preferred_generation:if next()%5==0 {99} else {4},..r};
        let answer=choose(request,11,&workers,1).unwrap();
        let eligible:Vec<_>=workers.iter().filter(|v|v.accepting&&v.domain==r.domain&&v.tenant==r.tenant).collect();
        let minimum=eligible.iter().min_by_key(|v|(v.pending,v.slot));
        match minimum {None=>assert_eq!(answer,None), Some(m)=> {
            assert!(eligible.iter().any(|v|Some(v.slot)==answer));
            if workers[0].accepting && request.preferred_generation==4 && workers[0].pending<=m.pending+1 {assert_eq!(answer,Some(0));}
            else {assert_eq!(answer,Some(m.slot));}
        }}
    }
}
#[test] fn bounded_slack_arithmetic() { let (r,mut w)=fixture(); w[0].pending=i32::MAX;w[1].pending=i32::MAX;assert_eq!(choose(r,11,&w,1024),Ok(Some(0))); }
