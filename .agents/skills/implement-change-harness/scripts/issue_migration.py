#!/usr/bin/env python3
"""One-time immutable-history inventory and restartable fork Issues migration.

Historical Markdown extraction is evidence only. Normal task reads use GitHub.
"""
import argparse
import copy
import json
import posixpath
from pathlib import Path
import re
import sys
import time
from github_issues import GitHub, IssueError, digest, need
from harness_state import Harness, Store, historical_inventory, encoded, git, require, repository_lock, state_path

MARKER = '<!-- fossilize-migration-v1:'


def section(text, start, stop):
    return text[text.index(start):text.index(stop, text.index(start))] if stop else text[text.index(start):]


def inventory(repo, source_sha):
    need(re.fullmatch('[0-9a-f]{40}', source_sha), 'immutable full historical source SHA required')
    store = Store(repo)
    tip = store.tip()
    records = store.records(tip)
    journal = json.loads(records['allocations.json'])
    registry, definitions, high = historical_inventory(repo, journal, source_sha)
    tasks = {}
    sources = {}
    for key, row in definitions.items():
        path = row['location']
        if path not in sources:
            sources[path] = git(repo, 'show', source_sha + ':' + path).decode()
        text = sources[path]
        starts = list(re.finditer(r'(?m)^(?:#{1,6}\s+|\d+\.\s+\*\*)(FOS-\d{6,})\s+[—–-]', text))
        start = next(i for i,match in enumerate(starts) if match[1] == key)
        begin = starts[start].start()
        end = starts[start+1].start() if start+1 < len(starts) else len(text)
        if path.endswith('observability-sweep.md'):
            end = min(end, text.index('## Dependency and delivery notes'))
        body = text[begin:end].rstrip() + '\n'
        if key == 'FOS-000025':
            # Its canonical paragraph explicitly incorporates the specification.
            body = text
        dependency_lines = [line for line in body.splitlines() if re.search(r'depends on',line,re.I)]
        dependencies = set(row['dependencies'])
        for line in dependency_lines:
            clause = re.split(r'depends on',line,flags=re.I)[1].lstrip(':* ').split(';')[0].split('**')[0]
            dependencies.update(re.findall(r'FOS-\d{6,}',clause))
        tasks[key] = dict(row, key=key, original=body, original_digest=digest(body),
                          dependencies=sorted(dependencies), dependency_reasons=dependency_lines,
                          group='observability' if int(key[4:]) <= 24 else 'harness',
                          disposition='individually migrated; original requirement retained')
    # Inventory every historical state/plan/provenance, not only current journal.
    artifact_facts = []
    allocated = set(registry) | {row['key'] for row in journal['issued']}
    visited = set()
    for commit in git(repo,'rev-list',tip).decode().splitlines():
        for path,content in store.records(commit).items():
            identity = digest(content.decode())
            if identity in visited or not path.endswith('.json'):
                continue
            visited.add(identity)
            value = json.loads(content)
            def inspect(value, location):
                if isinstance(value,dict):
                    if re.fullmatch(r'FOS-\d{6,}',str(value.get('key',''))):
                        allocated.add(value['key'])
                        need(value['key'] in tasks or 'retired' in registry.get(value['key'],{}).get('status',''), 'historical allocation/task requires explicit disposition: '+value['key'])
                    for field in ('subtasks','work_units'):
                        for unit in value.get(field,[]) if isinstance(value.get(field),list) else []:
                            key = unit.get('key','')
                            if re.fullmatch(r'FOS-\d{6,}',key):
                                allocated.add(key)
                                need(key in tasks or 'retired' in registry.get(key,{}).get('status',''), 'artifact-only task requires explicit disposition: ' + key)
                    for field,item in value.items():
                        if field not in ('original','records'):
                            inspect(item,location+'/'+field)
                elif isinstance(value,list):
                    for item in value: inspect(item,location)
            inspect(value,path)
            if path.endswith('/state.json'):
                artifact_facts.append({'path':path,'blob_digest':identity,'schema':value.get('schema_version'),
                                      'run_id':value.get('run_id'),'phase':value.get('phase')})
    retired = {key for key,row in registry.items() if 'retired' in row['status']}
    need(allocated <= set(tasks) | retired, 'unaccounted permanent identity')
    for key,task in tasks.items():
        need(set(task['dependencies']) <= set(tasks), 'missing dependency for ' + key)
    check_cycles({key:task['dependencies'] for key,task in tasks.items()})
    sweep = sources.get('local-docs/reviews/observability-sweep.md', '')
    shared = (section(sweep,'## Ordered implementation backlog','1. **FOS-') + '\n' + section(sweep,'## Dependency and delivery notes','## Adversarial recheck') + '\n' + section(sweep,'## Remaining verification',None)) if sweep else ''
    known_path = 'local-docs/known-issues/steam-shader-replay-observability.md'
    known = git(repo,'show',source_sha+':'+known_path).decode()
    document_inventory = []
    for path in git(repo,'ls-tree','-r','--name-only',source_sha,'local-docs').decode().splitlines():
        content = git(repo,'show',source_sha+':'+path)
        disposition = ('canonical task definitions extracted individually; research retained' if path in sources else
                       'permanent legacy allocation provenance' if path.endswith('backlog-keys.md') else
                       'unkeyed report: explicit overlap/external ownership above' if path==known_path else
                       'specification, research, guidance or sanitized fixture/evidence; no independent task definition')
        document_inventory.append({'path':path,'digest':digest(content.decode()),'disposition':disposition})
    delivery = {}
    for path, content in records.items():
        if not path.endswith('/state.json'):continue
        saved = json.loads(content)
        accepted = saved.get('final_acceptance')
        if saved.get('phase') != 'complete' or not accepted:continue
        report_path = path.removesuffix('state.json')+'reports/'+accepted['attempt_id']+'.json'
        need(report_path in records, 'historical final checker report missing')
        report = json.loads(records[report_path])
        need(report.get('verdict') == 'accepted' and all(report.get(field)==accepted[field] for field in ('candidate_sha','base_sha','plan_version')), 'historical acceptance tuple inconsistent')
        delivery[saved['task_key']] = {'state_path':path,'report_path':report_path,'report_digest':digest(report),
            'accepted':accepted,'pr':saved.get('publishing',{}).get('pr',{}).get('url'),
            'executed':report.get('executed',[]),'skipped':report.get('skipped',[])}
    return {'version':1,'execution_run':json.loads(records['active.json']),'repository':'robinwassen/Fossilize','source_sha':source_sha,'artifact_tip':tip,
            'tasks':tasks,'registry':registry,'retired_disposition':{key:'permanently retired; retained legacy allocation identity, not an active requirement' for key in retired},'registry_high_water':high,'historical_journal':journal,
            'document_inventory':document_inventory, 'artifact_state_inventory':artifact_facts, 'legacy_delivery_evidence':delivery, 'shared_observability_requirements':shared,
            'unkeyed':[{'path':known_path,'original':known,'disposition':
                'Fossilize obligations overlap FOS-000004/000005/000007/000009/000012/000013/000014/000022; retained individually without a duplicate issue. Steam scheduling/presentation belongs to external steam-for-linux#13810; no new Fossilize implementation requirement.'}],
            'research_disposition':'Specifications, reproduced E1..E9 findings, adversarial review and sanitized fixtures are supporting evidence, not additional task definitions.'}


def check_cycles(graph):
    done, visiting = set(), set()
    def visit(key):
        need(key in graph,'missing graph target')
        need(key not in visiting,'cyclic dependency graph')
        if key in done: return
        visiting.add(key)
        for dependency in graph[key]: visit(dependency)
        visiting.remove(key);done.add(key)
    for key in graph: visit(key)


def task_body(inv, key, parent):
    task=inv['tasks'][key]
    source=f"https://github.com/robinwassen/Fossilize/blob/{inv['source_sha']}/{task['location']}"
    links=[]
    for label,target in re.findall(r'\[([^\]]+)\]\(([^)]+)\)',task['original']):
        if '://' in target or target.startswith(('mailto:','/')):continue
        resolved=posixpath.normpath(posixpath.join(posixpath.dirname(task['location']),target)) if not target.startswith('#') else task['location']+target
        link=f"- [{label}](https://github.com/robinwassen/Fossilize/blob/{inv['source_sha']}/{resolved})"
        if link not in links:links.append(link)
    supporting='\n\n## Supporting links (resolved from the immutable original)\n\n'+'\n'.join(links) if links else ''
    return f"Legacy identity: `{key}`. Original source: [immutable historical definition]({source}).\n\nShared requirements and grouping: {parent}. All original criteria below remain required. Source status statements below describe their historical revision; current delivery evidence is recorded separately. Original relative links are resolved below.\n\n## Original definition (verbatim)\n\n"+task['original']+supporting


def mapping_context(inv, mapping):
    mapping_text='## Legacy identity mapping\n\n'+ '\n'.join(f'- `{key}` → {url}' for key,url in sorted(mapping.items()))
    mapping_text+='\n\nDependencies are native blocked-by edges. Every declared edge is required, including complete-propagation prerequisites. Numeric source order is sequencing advice; parent membership adds no blocking edge. A cancelled/not-planned prerequisite does not satisfy its obligation.\n\nCritical paths: '+ ' → '.join(mapping[f'FOS-{i:06}'] for i in (5,9))+' → ('+mapping['FOS-000010']+' and '+mapping['FOS-000011']+') → '+' → '.join(mapping[f'FOS-{i:06}'] for i in (12,13,14))+ '; persistence/capture: '+' → '.join(mapping[f'FOS-{i:06}'] for i in (3,16,17,19))+'.\n\n## Dependency reasons (verbatim metadata)\n\n'+'\n'.join(f"- `{key}`: "+' '.join(task['dependency_reasons']) for key,task in inv['tasks'].items() if task['dependencies'])
    return mapping_text


class Journal:
    """Only records-only evidence API is granted to the migration worker."""
    def __init__(self, repo, expected_run=None, root_issue=None, run_id=None, settle_wait=None):
        self.harness = Harness(repo, root_issue=root_issue, run_id=run_id)
        self.expected_run = expected_run
        self.settle_wait = settle_wait

    def records(self):
        tip = self.harness.store.tip()
        records = self.harness.store.records(tip)
        active = self.harness.select_pointer(records)
        need(active is not None,'migration needs active run')
        need(self.expected_run is None or active==self.expected_run,'migration belongs to another run; reconcile before any external write')
        state = json.loads(records[active['state']])
        return tip,records,state

    def get(self, name):
        tip,records,state=self.records()
        path=f"runs/{state['task_key']}/{state['run_id']}/evidence/{name}.json"
        return json.loads(records[path]) if path in records else None

    def _authorize_locked(self):
        tip,records,state=self.records()
        need(not state['limits']['exhausted'] and time.time()<state['limits']['deadline'], 'run budget exhausted; retain facts and ask coordinator for authorized extension')
        need(self.harness.artifact_sync(tip,records,adopt=True)['status']=='confirmed', 'external dispatch requires exact confirmed artifact tip')

    def guarded(self, action):
        # Only the live coordinator supplies this callback. Never call it while
        # holding the artifact lock: it owns the canonical settlement transaction.
        selected = bool(self.harness.selection)
        need(not selected or callable(self.settle_wait), 'selected journal requires coordinator settle_wait callback before external dispatch')
        while True:
            started = time.time()
            with repository_lock(self.harness.repo, blocking=selected) as waited:
                if not selected or not waited:
                    self._authorize_locked()
                    return action()
                receipt = self.harness.wait_receipt(started, waited)
            self.settle_wait(receipt)
            # Reload latest selected records and exact publication on reacquire.

    def authorize(self):
        return self.guarded(lambda: None)

    def dispatch(self, action):
        return self.guarded(action)

    def append(self, name, value):
        tip,records,state = self.records()
        path = f"runs/{state['task_key']}/{state['run_id']}/evidence/{name}.json"
        if path in records:
            need(json.loads(records[path]) == value,'journal record conflicts')
            # Replaying an identical record is inspection only. New appends and
            # authorize() retain the exact-publication barrier before any effect.
            return
        before = {path:content for path,content in records.items() if path.endswith('/state.json') or path in ('active.json','allocations.json')}
        result = self.harness.apply('records-only',{'records':{path:value}},tip,state['revision'])
        need(result['artifact_publication']['status']=='confirmed','intent/observation publication incomplete; exact retry: '+json.dumps(result['artifact_publication']['retry'],sort_keys=True))
        after = self.harness.store.records(result['tip'])
        need(all(after[path]==content for path,content in before.items() if not self.harness.selection or path == state_path(state['task_key'],state['run_id']) or path == 'allocations.json'),'restricted journal changed canonical bytes')
        if result.get('artifact_wait'):
            need(callable(self.settle_wait), 'selected journal wait requires coordinator settlement before dispatch')
            self.settle_wait(result['artifact_wait'])


class Migration:
    def __init__(self, repo, evidence, gh=None, journal=None, settle_wait=None):
        self.repo,self.inventory = repo,evidence
        self.gh,self.journal = gh or GitHub(),journal or Journal(repo,evidence.get('execution_run'),evidence.get('root_issue'),evidence.get('run_id'),settle_wait)
        self.mapping = {}
        self.rows = {}

    def operation(self, key, effect, expected, action, reconcile):
        op = digest({'source':self.inventory['source_sha'],'key':key,'effect':effect})
        intent = {'operation':op,'destination':'github.com/robinwassen/Fossilize','effect':effect,
                  'expected':expected,'marker':MARKER+key+' -->','next_action':'read complete authoritative service state, reconcile marker/identity, then dispatch only if absent'}
        self.journal.append('io-'+op+'-intent',intent)
        observed = reconcile()
        previous = (self.journal.get('io-'+op+'-returned') or self.journal.get('io-'+op+'-observed')) if hasattr(self.journal,'get') else None
        if observed is None and previous is not None:
            for delay in (.25,.5,1,2):
                time.sleep(delay)
                observed=reconcile()
                if observed is not None:break
            need(observed is not None, 'previous returned identity is no longer visible; reconcile deletion/conflict before any mutation retry')
        if observed is None:
            try:
                returned = self.journal.dispatch(action) if hasattr(self.journal,'dispatch') else action()
            except IssueError:
                # A failed write may already have happened. Always reconcile;
                # incomplete/failed reads raise and never permit blind retries.
                observed = reconcile()
                if observed is None: raise
            else:
                def identities(value):
                    if isinstance(value,dict):
                        return {name:value[name] for name in ('id','node_id','number','html_url','state','state_reason') if name in value}
                    if isinstance(value,list):return [identities(item) for item in value]
                    return None
                self.journal.append('io-'+op+'-returned',{'operation':op,'returned_identity':identities(returned),'response_digest':digest(returned)})
            observed = reconcile()
            # GitHub's list indexes can lag a successful create. Retry reads only;
            # response loss, authentication failure or ambiguity never retries a
            # mutation. Each response identity is already durably published.
            for delay in (.25,.5,1,2):
                if observed is not None:break
                time.sleep(delay)
                observed = reconcile()
        need(observed is not None,'uncertain external effect; re-run same operation to reconcile')
        self.journal.append('io-'+op+'-observed',{'operation':op,'observation':observed})
        return observed

    def definition(self,key,title,body):
        body = body.rstrip()+'\n\n'+MARKER+key+' -->\n'
        def reconcile():
            matches = [row for row in self.gh.issues() if MARKER+key+' -->' in (row.get('body') or '')]
            need(len(matches)<=1,'multiple migration markers; human reconciliation required')
            if not matches:return None
            row = matches[0]
            need(row['title']==title and row['body']==body,'issue edited or conflicting definition; preserve user edits and reconcile')
            return {name:row[name] for name in ('id','node_id','number','html_url','title','body')}
        row = self.operation(key,{'create_issue':title,'body_digest':digest(body)},'absent or exact marker/body',
                             lambda:self.gh.api('issues','POST',{'title':title,'body':body}),reconcile)
        self.mapping[key]=row['html_url'];self.rows[key]=row
        return row

    def comment(self,key,number,body):
        marker = MARKER+key+' -->'
        body = body.rstrip()+'\n\n'+marker+'\n'
        def reconcile():
            matches=[row for row in self.gh.comments(number) if marker in (row.get('body') or '')]
            need(len(matches)<=1,'ambiguous managed comment')
            if not matches:return None
            need(matches[0]['body']==body,'managed comment edited; reconcile without overwriting user text')
            return {'id':matches[0]['id'],'body_digest':digest(body)}
        return self.operation(key,{'comment_on':number,'body_digest':digest(body)},'marked comment absent or exact',
                              lambda:self.gh.api(f'issues/{number}/comments','POST',{'body':body}),reconcile)

    def run(self):
        inv=self.inventory
        self.journal.append('migration-inventory',inv)
        # Native endpoint capability GET checks precede any mutation. A 404/403
        # remains a prerequisite failure; prose is not substituted for native edges.
        capability = self.gh.call(['api','--hostname','github.com','graphql','-f','query={__type(name:"Issue"){fields{name}}}'])
        need({'parent','subIssues','blockedBy'} <= {field['name'] for field in capability['data']['__type']['fields']}, 'native hierarchy/dependency capability unavailable')
        obs=self.definition('observability','Observability: preserve trustworthy outcomes across Fossilize',
            'Required outcome: deliver every migrated observability child with its original criteria. This epic stays open until all required children and shared gates complete.\n\n'+inv['shared_observability_requirements']+
            '\n\n## Unkeyed report disposition\n'+inv['unkeyed'][0]['original']+'\n'+inv['unkeyed'][0]['disposition']+'\n\n'+inv['research_disposition'])
        harness=self.definition('harness','Harness: independently verify and publish repository changes',
            'Required outcome: preserve independent role checks, exact candidate gates, bounded repair, restartable evidence and one implementation PR. Required deliverables are the migrated harness children; this parent stays open while any child remains incomplete. Historical delivery of individual children does not close this outcome.')
        for key,task in sorted(inv['tasks'].items()):
            parent=self.mapping[task['group']]
            body=task_body(inv,key,parent)
            self.definition(key,key+' — '+task['title'],body)
        # All definitions exist and have exact content before any relationship.
        for key in self.mapping:
            number=self.rows[key]['number']
            self.gh.api(f'issues/{number}/sub_issues?per_page=100',paginate=True)
            self.gh.api(f'issues/{number}/dependencies/blocked_by?per_page=100',paginate=True)
        for key,task in sorted(inv['tasks'].items()):
            row,parent=self.rows[key],self.rows[task['group']]
            def parent_reconcile(row=row,parent=parent):
                children=self.gh.api(f"issues/{parent['number']}/sub_issues?per_page=100",paginate=True)
                return {'parent':parent['html_url'],'child':row['html_url']} if any(item['id']==row['id'] for item in children) else None
            self.operation('parent-'+key,{'parent':parent['html_url'],'child':row['html_url']},'edge absent or present; preserve unrelated edges',
                lambda row=row,parent=parent:self.gh.api(f"issues/{parent['number']}/sub_issues",'POST',{'sub_issue_id':row['id']}),parent_reconcile)
            for dependency in task['dependencies']:
                blocker=self.rows[dependency]
                def edge_reconcile(row=row,blocker=blocker):
                    edges=self.gh.api(f"issues/{row['number']}/dependencies/blocked_by?per_page=100",paginate=True)
                    return {'dependent':row['html_url'],'blocked_by':blocker['html_url']} if any(item['id']==blocker['id'] for item in edges) else None
                self.operation('blocked-'+key+'-'+dependency,{'dependent':row['html_url'],'blocked_by':blocker['html_url']},'edge absent or present; preserve unrelated edges',
                    lambda row=row,blocker=blocker:self.gh.api(f"issues/{row['number']}/dependencies/blocked_by",'POST',{'issue_id':blocker['id']}),edge_reconcile)
        mapping_text=mapping_context(inv,self.mapping)
        self.comment('identity-map',obs['number'],mapping_text)
        self.comment('identity-map-harness',harness['number'],mapping_text)
        for key,pr in [('FOS-000025',2),('FOS-000026',5),('FOS-000027',6)]:
            merged=self.gh.call(['pr','view',str(pr),'--repo','robinwassen/Fossilize','--json','number,url,state,mergedAt,mergeCommit,title,headRefOid,baseRefName'])
            retained = inv['legacy_delivery_evidence'][key]
            need(retained['pr']==merged['url'] and retained['accepted']['candidate_sha']==merged['headRefOid'] and retained['executed'] and not retained['skipped'] and merged['baseRefName']=='master', 'merged PR head and independent historical acceptance must agree')
            need(merged['state']=='MERGED' and merged['mergedAt'] and merged['mergeCommit']['oid'],'legacy delivery requires actual merged PR')
            # Historical independent verification is retained in immutable artifacts.
            body=f"Delivered in {merged['url']}, merged {merged['mergedAt']} as `{merged['mergeCommit']['oid']}`. Historical independent checker `{retained['accepted']['attempt_id']}` accepted `{retained['accepted']['candidate_sha']}` with {len(retained['executed'])} executed tests and no skipped tests. Evidence remains on [harness-artifacts](https://github.com/robinwassen/Fossilize/tree/harness-artifacts/runs/{key}); these executions establish that historical implementation only, not later revisions. Original source status above is historical."
            self.comment('delivery-'+key,self.rows[key]['number'],body)
            row=self.rows[key]
            def closed(row=row):
                actual=self.gh.api(f"issues/{row['number']}")
                return {'url':actual['html_url'],'state':actual['state'],'state_reason':actual.get('state_reason')} if actual['state']=='closed' and actual.get('state_reason')=='completed' else None
            self.operation('close-'+key,{'close_completed':row['html_url'],'merged_pr':merged},'delivered merged implementation with retained independent evidence',
                lambda row=row:self.gh.api(f"issues/{row['number']}",'PATCH',{'state':'closed','state_reason':'completed'}),closed)
        result=audit(self.gh,inv)
        self.journal.append('migration-coverage-audit',result)
        return result


def audit(gh,inv):
    all_rows=gh.issues();mapping={};details={}
    expected_keys=set(inv['tasks'])|{'observability','harness'}
    for key in sorted(expected_keys):
        marker=MARKER+key+' -->'
        matches=[row for row in all_rows if marker in (row.get('body') or '')]
        need(len(matches)==1,'missing/duplicate migrated definition '+key)
        row=matches[0];mapping[key]=row['html_url'];details[key]=gh.issue(row['html_url'])
        if key in inv['tasks']:
            expected=task_body(inv,key,next(item['html_url'] for item in all_rows if MARKER+inv['tasks'][key]['group']+' -->' in (item.get('body') or ''))).rstrip()+'\n\n'+marker+'\n'
            need(row['body']==expected,'full original/context/body digest mismatch '+key)
            need(row['body'].endswith(marker+'\n'),'definition marker changed')
        need(row['state']==('closed' if key in ('FOS-000025','FOS-000026','FOS-000027') else 'open'),'dishonest delivery/epic status '+key)
        if row['state']=='closed':need(row.get('state_reason')=='completed','cancelled legacy task')
    for group,key in (('observability','identity-map'),('harness','identity-map-harness')):
        marker=MARKER+key+' -->'
        matches=[comment for comment in gh.comments(details[group]['number']) if marker in (comment.get('body') or '')]
        expected=mapping_context(inv,mapping).rstrip()+'\n\n'+marker+'\n'
        need(len(matches)==1 and matches[0]['body']==expected, 'complete mapping context/reasons/critical paths changed or missing')
    merged_delivery = {}
    for key,number in (('FOS-000025',2),('FOS-000026',5),('FOS-000027',6)):
        merged=gh.call(['pr','view',str(number),'--repo','robinwassen/Fossilize','--json','number,url,state,mergedAt,mergeCommit,title,headRefOid,baseRefName'])
        retained=inv['legacy_delivery_evidence'][key]
        need(merged['state']=='MERGED' and merged['baseRefName']=='master' and merged['mergedAt'] and merged['mergeCommit']['oid'] and retained['accepted']['candidate_sha']==merged['headRefOid'] and retained['pr']==merged['url'] and retained['executed'] and not retained['skipped'], 'legacy merge/checker evidence mismatch')
        comments=gh.comments(details[key]['number'])
        need(any(MARKER+'delivery-'+key+' -->' in comment.get('body','') and merged['url'] in comment['body'] and merged['mergeCommit']['oid'] in comment['body'] for comment in comments), 'missing honest legacy delivery evidence')
        merged_delivery[key]=merged
    obs=details['observability']
    need(inv['shared_observability_requirements'] in obs['body'],'shared acceptance text lost')
    for key,task in inv['tasks'].items():
        row=details[key]
        need(row['parent']==mapping[task['group']],'wrong native parent '+key)
        need(row['blocked_by']==sorted(mapping[target] for target in task['dependencies']),'missing/unexpected native dependency '+key)
    for group in ('observability','harness'):
        need(details[group]['children']==sorted(mapping[key] for key,task in inv['tasks'].items() if task['group']==group),'parent child coverage mismatch')
    check_cycles({key:task['dependencies'] for key,task in inv['tasks'].items()})
    return {'repository':inv['repository'],'source_sha':inv['source_sha'],'mapping':mapping,
            'tasks':len(inv['tasks']),'epics':2,'edges':sum(len(task['dependencies']) for task in inv['tasks'].values()),
            'snapshots':details,'merged_delivery':merged_delivery,'result':'passed','coverage':'Full original bodies/shared context/status/native hierarchy/dependencies; every key individually retained.'}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=('inventory','run','audit'))
    parser.add_argument('--repo',required=True);parser.add_argument('--source-sha');parser.add_argument('--inventory');parser.add_argument('--output')
    args=parser.parse_args()
    try:
        inv=inventory(args.repo,args.source_sha) if args.action=='inventory' else json.loads(Path(args.inventory).read_text())
        result=inv if args.action=='inventory' else Migration(args.repo,inv).run() if args.action=='run' else audit(GitHub(),inv)
        if args.output:Path(args.output).write_bytes(encoded(result))
        else:print(encoded(result).decode(),end='')
    except (IssueError,ValueError,OSError,KeyError) as error:
        parser.exit(2,str(error)+'\nResume: rerun the exact action after reconciling the last committed external-operation intent and authoritative gh readback.\n')

if __name__=='__main__':main()
