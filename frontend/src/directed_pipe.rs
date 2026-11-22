//! Typed compile-time continuous DATA graph planner. No native guest
//! transport, PipeHandle, authorized ActorRef or supervisor CONTROL here.
//! The frontend must supply trusted, nominally checked endpoint metadata.
use std::collections::HashSet;
use crate::diag::Pos;
use crate::lexer::{Tok, Token};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Direction { Pipe, Rpipe }
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Endpoint { Actor, Channel, StreamReadable, StreamSink, StreamDuplex }
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Stage {
    pub id: String,
    pub tenant: String,
    pub endpoint: Endpoint,
    pub input: Option<String>,
    pub output: Option<String>,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Edge {
    pub source_index: usize,
    pub sink_index: usize,
    pub source: String,
    pub sink: String,
    pub payload: String,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Plan {
    pub written: Vec<Stage>,
    pub flow_indices: Vec<usize>,
    pub edges: Vec<Edge>,
    pub direction: Direction,
    pub tenant: String,
}
fn label(raw: &str) -> bool {
    !raw.is_empty() && raw.len() <= 256
        && raw.as_bytes()[0].is_ascii_alphabetic()
        && raw.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
}
fn type_label(raw: &str) -> bool {
    !raw.is_empty() && raw.len() <= 256
        && raw.as_bytes()[0].is_ascii_alphabetic()
        && raw.bytes().all(|b|
            b.is_ascii_alphanumeric() || b == b'_' || b == b'.' || b == b'<' || b == b'>')
}
pub fn plan(written: Vec<Stage>, direction: Direction) -> Result<Plan, &'static str> {
    if !(2..=16).contains(&written.len()) { return Err("E_PIPE_STAGE_LIMIT"); }
    let tenant = written[0].tenant.clone();
    let mut seen = HashSet::new();
    for stage in &written {
        if !label(&stage.id) || !label(&stage.tenant) { return Err("E_PIPE_ID"); }
        if stage.tenant != tenant { return Err("E_PIPE_TENANT_BOUNDARY"); }
        if !seen.insert(stage.id.as_str()) { return Err("E_PIPE_DUPLICATE_STAGE"); }
        if stage.input.as_deref().is_some_and(|x| !type_label(x))
            || stage.output.as_deref().is_some_and(|x| !type_label(x)) {
            return Err("E_PIPE_TYPE_LABEL");
        }
        match stage.endpoint {
            Endpoint::Actor => {
                if stage.input.is_none() && stage.output.is_none() {
                    return Err("E_PIPE_PORTS");
                }
            }
            Endpoint::Channel | Endpoint::StreamDuplex => {
                if stage.input.is_none() || stage.input != stage.output {
                    return Err("E_PIPE_CHANNEL_PORTS");
                }
            }
            Endpoint::StreamReadable => {
                if stage.input.is_some() || stage.output.is_none() {
                    return Err("E_PIPE_READONLY_PORT");
                }
            }
            Endpoint::StreamSink => {
                if stage.input.is_none() || stage.output.is_some() {
                    return Err("E_PIPE_WRITEONLY_PORT");
                }
            }
        }
    }
    let mut flow_indices: Vec<usize> = (0..written.len()).collect();
    if direction == Direction::Rpipe { flow_indices.reverse(); }
    let mut edges = Vec::with_capacity(written.len() - 1);
    for pair in flow_indices.windows(2) {
        let a = &written[pair[0]];
        let b = &written[pair[1]];
        let output = a.output.as_ref().ok_or("E_PIPE_MISSING_EMITS")?;
        let input = b.input.as_ref().ok_or("E_PIPE_MISSING_RECEIVES")?;
        if input != output { return Err("E_PIPE_TYPE_MISMATCH"); }
        edges.push(Edge { source_index: pair[0], sink_index: pair[1],
            source: a.id.clone(), sink: b.id.clone(), payload: output.clone() });
    }
    Ok(Plan { written, flow_indices, edges, direction, tenant })
}
pub fn check_unicast(plans: &[Plan]) -> Result<(), &'static str> {
    if plans.len() > 64 { return Err("E_PIPE_PLAN_LIMIT"); }
    let mut edges = HashSet::new();
    let mut outputs = HashSet::new();
    let mut inputs = HashSet::new();
    for p in plans {
        for e in &p.edges {
            let source = (&p.tenant, &e.source);
            if !edges.insert((source, &e.sink)) { return Err("E_PIPE_DUPLICATE_EDGE"); }
            if !outputs.insert(source) { return Err("E_PIPE_OUTPUT_OCCUPIED"); }
            // No implicit fan-in: each sink port has one upstream owner.
            if !inputs.insert((&p.tenant, &e.sink)) { return Err("E_PIPE_INPUT_OCCUPIED"); }
        }
    }
    Ok(())
}
/// The real lexer, not raw-source scanning, identifies unsupported continuous
/// graph keywords. Comments and strings do not mint runtime capabilities.
pub fn unlowered_pos(tokens: &[Token]) -> Option<Pos> {
    tokens.iter().find_map(|t| match &t.tok {
        Tok::Ident(name) if name == "pipe" || name == "rpipe" => Some(t.pos),
        _ => None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    fn actor(id: &str, input: Option<&str>, output: Option<&str>) -> Stage {
        Stage { id: id.into(), tenant: "tenant_a".into(), endpoint: Endpoint::Actor,
            input: input.map(str::to_owned), output: output.map(str::to_owned) }
    }
    fn ch(id: &str) -> Stage {
        Stage { id:id.into(), tenant:"tenant_a".into(),endpoint:Endpoint::Channel,
            input:Some("int".into()),output:Some("int".into()) }
    }
    #[test]
    fn reverse_written_and_forward_graphs_have_same_data_edges() {
        let p=actor("producer",Some("int"),Some("int"));
        let c=actor("consumer",Some("int"),None);
        let a=plan(vec![p.clone(),ch("channel"),c.clone()],Direction::Pipe).unwrap();
        let b=plan(vec![c,ch("channel"),p],Direction::Rpipe).unwrap();
        let edges=|p: &Plan| p.edges.iter().map(|e| (&e.source,&e.sink,&e.payload))
            .collect::<Vec<_>>();
        assert_eq!(edges(&a),edges(&b));
        assert_eq!(a.flow_indices,vec![0,1,2]);
        assert_eq!(b.flow_indices,vec![2,1,0]);
        assert_eq!(b.written[0].id,"consumer");
        assert_eq!(check_unicast(&[a,b]),Err("E_PIPE_DUPLICATE_EDGE"));
    }
    #[test]
    fn port_type_tenant_duplicate_and_limit_rejections() {
        let p=actor("producer",Some("int"),Some("int"));
        let c=actor("consumer",Some("int"),None);
        assert_eq!(plan(vec![c.clone(),p.clone()],Direction::Pipe),
            Err("E_PIPE_MISSING_EMITS"));
        assert_eq!(plan(vec![p.clone(),c.clone()],Direction::Rpipe),
            Err("E_PIPE_MISSING_EMITS"));
        assert_eq!(plan(vec![p.clone(),actor("sink",Some("bool"),None)],Direction::Pipe),
            Err("E_PIPE_TYPE_MISMATCH"));
        let mut foreign=c.clone(); foreign.tenant="tenant_b".into();
        assert_eq!(plan(vec![p.clone(),foreign],Direction::Pipe),
            Err("E_PIPE_TENANT_BOUNDARY"));
        assert_eq!(plan(vec![p.clone(),p.clone()],Direction::Pipe),
            Err("E_PIPE_DUPLICATE_STAGE"));
        assert_eq!(plan(vec![p.clone()],Direction::Pipe),Err("E_PIPE_STAGE_LIMIT"));
        assert_eq!(plan(vec![p.clone();17],Direction::Rpipe),Err("E_PIPE_STAGE_LIMIT"));
        let upstream = actor("upstream", Some("int"), Some("int"));
        let common_sink = actor("common_sink", Some("int"), None);
        let f = plan(vec![p.clone(),common_sink.clone()],Direction::Pipe).unwrap();
        let r = plan(vec![common_sink,upstream],Direction::Rpipe).unwrap();
        assert_eq!(check_unicast(&[f,r]),Err("E_PIPE_INPUT_OCCUPIED"));
        let mut bad=p;bad.id="bad\u{202e}".into();
        assert_eq!(plan(vec![bad,c],Direction::Pipe),Err("E_PIPE_ID"));
    }
}
