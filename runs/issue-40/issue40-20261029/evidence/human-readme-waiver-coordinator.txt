"""One run's explicit human exception, never a change to the shipped helper."""
import hashlib,json

RECORD='runs/issue-40/issue40-20261029/evidence/human-readme-only-base-waiver.json'
OLD='5eb2c496dde819c868fa3d7d1e52c51047d9adb8'
NEW='2ffeaae69e4e127dcb0bfb3bf6319c6599509546'

def bind(h):
    from harness_state import Invalid,require,git
    original=h.publication_guard
    def guard(state,payload):
        try:return original(state,payload)
        except Invalid as error:
            if str(error)!='target base moved; integrate and recheck':raise
        # The ordinary guard already checked task, budget, candidate, approvals,
        # findings, readiness and every publication destination field.
        r=state['repository']
        require(h.root_issue=='https://github.com/robinwassen/Fossilize/issues/40' and h.run_id=='issue40-20261029','README waiver selector differs')
        records=h.store.records(h.store.tip())
        require(RECORD in records,'explicit human README waiver absent')
        waiver=json.loads(records[RECORD])
        require(waiver['kind']=='human-directed-verification-waiver' and waiver['human_instruction']=='Skip reverfiication since its just a readme change','human waiver differs')
        require(r['base_sha']==waiver['accepted_original_base']==OLD and waiver['observed_target_master']==NEW,'README waiver exact bases differ')
        actual=git(h.repo,'ls-remote',r['target_remote'],'refs/heads/'+r['target_branch']).decode().split()
        require(actual==[NEW,'refs/heads/master'],'further target advance is not waived')
        require(git(h.repo,'merge-base',OLD,NEW).decode().strip()==OLD,'waived README target is not descendant')
        require(git(h.repo,'diff','--name-only',OLD,NEW).decode().splitlines()==waiver['changed_paths']==['README.md'],'waived target change is not exactly README')
        diff=git(h.repo,'diff',OLD,NEW,'--','README.md')
        require(hashlib.sha256(diff).hexdigest()==waiver['diff_sha256'] and diff.decode()==waiver['diff'],'README diff differs from reviewed human waiver')
        require(not git(h.repo,'diff',OLD,r['candidate_sha'],'--','README.md').strip(),'candidate changes README; user waiver insufficient')
        # Preserve the remainder of the ordinary guard after its target-base check.
        if state['publishing']['stage'] not in ('none','push'):
            head=git(h.repo,'ls-remote',r['target_remote'],'refs/heads/'+r['branch']).decode().split()
            require(head==[r['candidate_sha'],'refs/heads/'+r['branch']],'remote head moved; never force push')
    h.publication_guard=guard
