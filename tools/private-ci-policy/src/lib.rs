//! Source-free, deterministic policy oracle for Oreslang private CI.
//!
//! This does not call GitHub, mint capabilities, authorize an export, or
//! replace the live publisher's independently reviewed security controls.

use std::collections::BTreeSet;

pub const SOURCE_NAME: &str = "ores-truffle-oreslang/oreslang-source.llvm";
pub const SOURCE_ID: u64 = 1_410_897_874;
pub const SECURITY_REVIEWER_ID: u64 = 11_139_560;
pub const P0_INCIDENTS: [(u64, u64); 9] = [
    (22, 5_832_921_751),
    (77, 5_904_679_716),
    (127, 5_935_338_521),
    (171, 6_042_555_082),
    (172, 6_042_602_777),
    (198, 6_070_465_058),
    (200, 6_071_264_936),
    (212, 6_087_989_978),
    (222, 6_092_501_448),
];

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Denied {
    SourceIdentity,
    CarrierIdentity,
    SourceProtection,
    CarrierProtection,
    UnpinnedCheck,
    SourceReview,
    UnapprovedDestination,
    Containment,
    GovernanceDrift,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Repository {
    pub name: String,
    pub id: u64,
    pub private: bool,
    pub visibility_private: bool,
    pub archived: bool,
    pub disabled: bool,
    pub fork: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RequiredCheck {
    pub context: String,
    pub app_id: u64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProtectedMain {
    pub sha: String,
    pub protected: bool,
    pub codeowner_reviews: bool,
    pub dismiss_stale: bool,
    pub last_push_approval: bool,
    pub approvals: u32,
    pub conversations_resolved: bool,
    pub strict_checks: bool,
    pub checks: Vec<RequiredCheck>,
    pub legacy_contexts: Vec<String>,
    pub administrators_enforced: bool,
    pub review_bypass_allowed: bool,
    pub force_pushes_allowed: bool,
    pub branch_deletion_allowed: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SourcePr {
    pub number: u64,
    pub open: bool,
    pub draft: bool,
    pub author_id: u64,
    pub head_sha: String,
    pub head_repo_id: u64,
    pub base_repo_id: u64,
    pub base_branch: String,
    pub reviewer_id: u64,
    pub approved: bool,
    pub approved_sha: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ClosureReason {
    Completed,
    NotPlanned,
    Unknown,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Incident {
    pub number: u64,
    pub immutable_id: u64,
    pub closed: bool,
    pub reason: ClosureReason,
}

/// Model of an independently captured read-only GitHub observation.
/// Constructing a Snapshot alone is *never* an export authorization.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Snapshot {
    pub source: Repository,
    pub carrier: Repository,
    pub source_main: ProtectedMain,
    pub carrier_main: ProtectedMain,
    pub source_pr: SourcePr,
    pub allowlisted_repositories: Vec<String>,
    pub incidents: Vec<Incident>,
    pub retained_objects_cleared: bool,
}

/// Values pinned by an independent trusted policy, not a PR under test.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Expected {
    pub carrier_name: String,
    pub carrier_id: u64,
    pub carrier_main_sha: String,
    pub source_main_sha: String,
    pub source_pr_number: u64,
    pub source_pr_sha: String,
}

fn sha40(value: &str) -> bool {
    value.len() == 40 && value.bytes().all(|b| b.is_ascii_hexdigit() && !b.is_ascii_uppercase())
}

fn valid_repository_name(name: &str) -> bool {
    let mut parts = name.split('/');
    let valid_part = |part: &str| {
        !part.is_empty()
            && part.as_bytes()[0].is_ascii_alphanumeric()
            && part.bytes().all(|b| b.is_ascii_alphanumeric() || b"_.-".contains(&b))
    };
    matches!((parts.next(), parts.next(), parts.next()),
        (Some(owner), Some(repo), None) if valid_part(owner) && valid_part(repo))
}

fn private_repo(repo: &Repository) -> bool {
    repo.id > 0 && repo.private && repo.visibility_private
        && !repo.archived && !repo.disabled && !repo.fork
}

fn protection(main: &ProtectedMain) -> Result<(), Denied> {
    if !sha40(&main.sha) || !main.protected || !main.codeowner_reviews
        || !main.dismiss_stale || !main.last_push_approval || main.approvals == 0
        || !main.conversations_resolved || !main.strict_checks
        || !main.administrators_enforced || main.review_bypass_allowed
        || main.force_pushes_allowed || main.branch_deletion_allowed
    {
        return Err(Denied::SourceProtection);
    }
    if main.checks.is_empty() {
        return Err(Denied::UnpinnedCheck);
    }
    let mut checks = BTreeSet::new();
    for check in &main.checks {
        if check.context.trim() != check.context || check.context.is_empty()
            || check.app_id == 0 || !checks.insert(check.context.as_str())
        {
            return Err(Denied::UnpinnedCheck);
        }
    }
    let mut legacy = BTreeSet::new();
    for name in &main.legacy_contexts {
        if name.trim() != name || name.is_empty() || !legacy.insert(name.as_str())
            || !checks.contains(name.as_str())
        {
            return Err(Denied::UnpinnedCheck);
        }
    }
    Ok(())
}

fn validate(snapshot: &Snapshot, pin: &Expected) -> Result<(), Denied> {
    if snapshot.source.name != SOURCE_NAME || snapshot.source.id != SOURCE_ID
        || !private_repo(&snapshot.source)
    {
        return Err(Denied::SourceIdentity);
    }
    if !valid_repository_name(&pin.carrier_name) || pin.carrier_name == SOURCE_NAME
        || pin.carrier_name != snapshot.carrier.name
        || pin.carrier_id != snapshot.carrier.id || pin.carrier_id == SOURCE_ID
        || !private_repo(&snapshot.carrier)
    {
        return Err(Denied::CarrierIdentity);
    }
    protection(&snapshot.source_main).map_err(|error| match error {
        Denied::UnpinnedCheck => Denied::UnpinnedCheck,
        _ => Denied::SourceProtection,
    })?;
    if !sha40(&pin.source_main_sha) || snapshot.source_main.sha != pin.source_main_sha {
        return Err(Denied::SourceProtection);
    }
    protection(&snapshot.carrier_main).map_err(|error| match error {
        Denied::UnpinnedCheck => Denied::UnpinnedCheck,
        _ => Denied::CarrierProtection,
    })?;
    if !sha40(&pin.carrier_main_sha) || snapshot.carrier_main.sha != pin.carrier_main_sha {
        return Err(Denied::CarrierProtection);
    }
    let pr = &snapshot.source_pr;
    if pr.number != pin.source_pr_number || pr.number == 0
        || !pr.open || pr.draft || pr.author_id == 0
        || pr.author_id == SECURITY_REVIEWER_ID || !sha40(&pin.source_pr_sha)
        || pr.head_sha != pin.source_pr_sha || pr.head_repo_id != SOURCE_ID
        || pr.base_repo_id != SOURCE_ID || pr.base_branch != "main"
        || pr.reviewer_id != SECURITY_REVIEWER_ID || !pr.approved
        || pr.approved_sha != pin.source_pr_sha
    {
        return Err(Denied::SourceReview);
    }
    if snapshot.allowlisted_repositories.len() != 1
        || snapshot.allowlisted_repositories[0] != pin.carrier_name
    {
        return Err(Denied::UnapprovedDestination);
    }
    if !snapshot.retained_objects_cleared || snapshot.incidents.len() != P0_INCIDENTS.len() {
        return Err(Denied::Containment);
    }
    let mut visited = BTreeSet::new();
    for incident in &snapshot.incidents {
        if !visited.insert(incident.number)
            || !incident.closed || incident.reason != ClosureReason::Completed
            || !P0_INCIDENTS.contains(&(incident.number, incident.immutable_id))
        {
            return Err(Denied::Containment);
        }
    }
    Ok(())
}

/// Source-free policy equivalence check. The result is not an export permit.
/// The real exporter must obtain the observations independently and recheck
/// them immediately before each Git-object write and authorization ref.
pub fn verify_source_free(before: &Snapshot, after: &Snapshot, pin: &Expected) -> Result<(), Denied> {
    validate(before, pin)?;
    validate(after, pin)?;
    if before != after {
        return Err(Denied::GovernanceDrift);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture() -> (Snapshot, Expected) {
        let source = Repository {
            name: SOURCE_NAME.into(), id: SOURCE_ID, private: true,
            visibility_private: true, archived: false, disabled: false, fork: false,
        };
        let carrier = Repository {
            name: "example/carrier".into(), id: 42, ..source.clone()
        };
        let main = ProtectedMain {
            sha: "a".repeat(40), protected: true, codeowner_reviews: true,
            dismiss_stale: true, last_push_approval: true, approvals: 1,
            conversations_resolved: true, strict_checks: true,
            checks: vec![RequiredCheck { context: "security".into(), app_id: 15368 }],
            legacy_contexts: vec!["security".into()], administrators_enforced: true,
            review_bypass_allowed: false, force_pushes_allowed: false,
            branch_deletion_allowed: false,
        };
        let carrier_main = ProtectedMain { sha: "b".repeat(40), ..main.clone() };
        let pr = SourcePr {
            number: 100, open: true, draft: false, author_id: 10,
            head_sha: "c".repeat(40), head_repo_id: SOURCE_ID,
            base_repo_id: SOURCE_ID, base_branch: "main".into(),
            reviewer_id: SECURITY_REVIEWER_ID, approved: true,
            approved_sha: "c".repeat(40),
        };
        let incident_rows = P0_INCIDENTS.iter().map(|&(number, immutable_id)| Incident {
            number, immutable_id, closed: true, reason: ClosureReason::Completed,
        }).collect();
        let expected = Expected {
            carrier_name: carrier.name.clone(), carrier_id: carrier.id,
            carrier_main_sha: carrier_main.sha.clone(),
            source_main_sha: main.sha.clone(),
            source_pr_number: pr.number, source_pr_sha: pr.head_sha.clone(),
        };
        (Snapshot {
            source, carrier, source_main: main, carrier_main, source_pr: pr,
            allowlisted_repositories: vec!["example/carrier".into()],
            incidents: incident_rows, retained_objects_cleared: true,
        }, expected)
    }

    #[test]
    fn accepted_synthetic_policy() {
        let (snapshot, pin) = fixture();
        assert_eq!(verify_source_free(&snapshot, &snapshot, &pin), Ok(()));
    }

    #[test]
    fn source_identity_is_pinned() {
        let (mut s, p) = fixture();
        s.source.id = 7;
        assert_eq!(verify_source_free(&s, &s, &p), Err(Denied::SourceIdentity));
        s.source.id = SOURCE_ID;
        s.source.visibility_private = false;
        assert_eq!(verify_source_free(&s, &s, &p), Err(Denied::SourceIdentity));
    }

    #[test]
    fn destination_identity_and_allowlist_are_pinned() {
        let (mut s, p) = fixture();
        s.carrier.id += 1;
        assert_eq!(verify_source_free(&s, &s, &p), Err(Denied::CarrierIdentity));
        let (mut s, p) = fixture();
        s.allowlisted_repositories.clear();
        assert_eq!(verify_source_free(&s, &s, &p), Err(Denied::UnapprovedDestination));
        let (mut s, p) = fixture();
        s.allowlisted_repositories.push("other/repo".into());
        assert_eq!(verify_source_free(&s, &s, &p), Err(Denied::UnapprovedDestination));
    }

    #[test]
    fn protection_requires_every_gate() {
        let (s, p) = fixture();
        let mutations: [fn(&mut ProtectedMain); 11] = [
            |x: &mut ProtectedMain| x.protected = false,
            |x: &mut ProtectedMain| x.codeowner_reviews = false,
            |x: &mut ProtectedMain| x.dismiss_stale = false,
            |x: &mut ProtectedMain| x.last_push_approval = false,
            |x: &mut ProtectedMain| x.approvals = 0,
            |x: &mut ProtectedMain| x.conversations_resolved = false,
            |x: &mut ProtectedMain| x.strict_checks = false,
            |x: &mut ProtectedMain| x.administrators_enforced = false,
            |x: &mut ProtectedMain| x.review_bypass_allowed = true,
            |x: &mut ProtectedMain| x.force_pushes_allowed = true,
            |x: &mut ProtectedMain| x.branch_deletion_allowed = true,
        ];
        for mutate in mutations {
            let mut changed = s.clone();
            mutate(&mut changed.source_main);
            assert_eq!(verify_source_free(&changed, &changed, &p), Err(Denied::SourceProtection));
            let mut changed = s.clone();
            mutate(&mut changed.carrier_main);
            assert_eq!(verify_source_free(&changed, &changed, &p), Err(Denied::CarrierProtection));
        }
    }

    #[test]
    fn checks_must_be_app_pinned_and_unique() {
        let (s, p) = fixture();
        let mutations: [fn(&mut ProtectedMain); 6] = [
            |x: &mut ProtectedMain| x.checks.clear(),
            |x: &mut ProtectedMain| x.checks[0].app_id = 0,
            |x: &mut ProtectedMain| x.checks[0].context = "".into(),
            |x: &mut ProtectedMain| x.legacy_contexts.push("unbound".into()),
            |x: &mut ProtectedMain| { let item = x.checks[0].clone(); x.checks.push(item); },
            |x: &mut ProtectedMain| x.legacy_contexts.push("security".into()),
        ];
        for mutate in mutations {
            let mut changed = s.clone();
            mutate(&mut changed.source_main);
            assert_eq!(verify_source_free(&changed, &changed, &p), Err(Denied::UnpinnedCheck));
        }
    }

    #[test]
    fn exact_head_review_is_required() {
        let (s, p) = fixture();
        let mutations: [fn(&mut SourcePr); 8] = [
            |x: &mut SourcePr| x.draft = true,
            |x: &mut SourcePr| x.open = false,
            |x: &mut SourcePr| x.head_repo_id += 1,
            |x: &mut SourcePr| x.base_repo_id += 1,
            |x: &mut SourcePr| x.reviewer_id = 123,
            |x: &mut SourcePr| x.approved = false,
            |x: &mut SourcePr| x.approved_sha = "d".repeat(40),
            |x: &mut SourcePr| x.author_id = SECURITY_REVIEWER_ID,
        ];
        for mutate in mutations {
            let mut changed = s.clone();
            mutate(&mut changed.source_pr);
            assert_eq!(verify_source_free(&changed, &changed, &p), Err(Denied::SourceReview));
        }
    }

    #[test]
    fn all_nine_exposure_incidences_must_be_completed() {
        let (s, p) = fixture();
        for n in 0..P0_INCIDENTS.len() {
            let mut changed = s.clone();
            changed.incidents[n].closed = false;
            assert_eq!(verify_source_free(&changed, &changed, &p), Err(Denied::Containment));
            let mut changed = s.clone();
            changed.incidents[n].reason = ClosureReason::NotPlanned;
            assert_eq!(verify_source_free(&changed, &changed, &p), Err(Denied::Containment));
            let mut changed = s.clone();
            changed.incidents[n].immutable_id += 1;
            assert_eq!(verify_source_free(&changed, &changed, &p), Err(Denied::Containment));
        }
        let mut changed = s.clone();
        changed.incidents.pop();
        assert_eq!(verify_source_free(&changed, &changed, &p), Err(Denied::Containment));
        let mut changed = s.clone();
        changed.retained_objects_cleared = false;
        assert_eq!(verify_source_free(&changed, &changed, &p), Err(Denied::Containment));
    }

    #[test]
    fn mutable_policy_drift_is_denied() {
        let (s, p) = fixture();
        let mut after = s.clone();
        after.carrier_main.checks[0].app_id = 42;
        assert_eq!(verify_source_free(&s, &after, &p), Err(Denied::GovernanceDrift));
        let mut after = s.clone();
        after.source_main.approvals = 2;
        assert_eq!(verify_source_free(&s, &after, &p), Err(Denied::GovernanceDrift));
        let mut after = s.clone();
        after.source_main.sha = "e".repeat(40);
        assert_eq!(verify_source_free(&s, &after, &p), Err(Denied::SourceProtection));
    }

    #[test]
    fn no_open_incident_can_be_skipped() {
        let (mut s, p) = fixture();
        s.incidents.retain(|incident| incident.number != 222);
        assert_eq!(verify_source_free(&s, &s, &p), Err(Denied::Containment));
    }
}
