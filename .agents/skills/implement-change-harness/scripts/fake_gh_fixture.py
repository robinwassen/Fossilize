#!/usr/bin/env python3
"""Disposable subprocess service fixture. Never used by the production transport."""
import json
import os
from pathlib import Path
import sys

path=Path(os.environ['FAKE_GH_STATE']);state=json.loads(path.read_text());args=sys.argv[1:]
state.setdefault('calls',[]).append(args)
def save():path.write_text(json.dumps(state))
def emit(value):save();print(json.dumps(value));sys.exit(0)
def fail(message):save();print(message,file=sys.stderr);sys.exit(1)
if state.pop('auth_failure',False):fail('authentication failed')
if args[:2]==['pr','view']:
 n=int(args[2].rstrip('/').split('/')[-1]);
 if str(n) in state.get('prs',{}):emit(state['prs'][str(n)])
 emit({'number':n,'url':f'https://github.com/robinwassen/Fossilize/pull/{n}','state':'MERGED','headRefOid':'b'*40,'baseRefName':'master','mergedAt':'2026-10-27T00:00:00Z','mergeCommit':{'oid':'a'*40},'title':'Delivered'})
if 'graphql' in args:
 if any('__type' in arg for arg in args):emit({'data':{'__type':{'fields':[{'name':name} for name in ('parent','subIssues','blockedBy')]}}})
 n=int(next(arg for arg in args if arg.startswith('number=')).split('=')[1]);row=state['issues'].get(str(n))
 if row is None:fail('not found')
 parent=next((other for other in state['issues'].values() if row['id'] in other.get('children',[])),None)
 emit({'data':{'repository':{'issue':{'parent':{'url':parent['html_url']} if parent else None}}}})
endpoint=next((arg for arg in args if arg.startswith('repos/')),None)
if endpoint is None or not endpoint.startswith('repos/robinwassen/Fossilize/'):fail('wrong explicit repository')
resource=endpoint.split('repos/robinwassen/Fossilize/',1)[1].split('?')[0]
method=args[args.index('--method')+1] if '--method' in args else 'GET'
payload=json.load(sys.stdin) if '--input' in args else {}
parts=resource.split('/');issues=state.setdefault('issues',{})
def paginated(rows):
 # Multiple pages exercise flattening; every read observes full remote service.
 return [rows[:1],rows[1:]] if '--slurp' in args else rows
if resource=='issues':
 if method=='GET':emit(paginated(list(issues.values())))
 n=max([7]+[int(key) for key in issues])+1
 row={'id':1000+n,'node_id':'I_'+str(n),'number':n,'title':payload['title'],'body':payload['body'],
      'html_url':f'https://github.com/robinwassen/Fossilize/issues/{n}','state':'open','state_reason':None,'children':[],'blocked':[]}
 issues[str(n)]=row
 if state.pop('malformed_create',False):
  save();print('invalid response');sys.exit(0)
 if state.pop('timeout_create',False):
  save();__import__('time').sleep(2)
 if state.pop('lose_create_response',False):fail('lost response after committed create')
 emit(row)
if len(parts)>=2 and parts[0]=='issues':
 row=issues.get(parts[1]);
 if row is None:fail('not found')
 if len(parts)==2:
  if method=='PATCH':
   row.update(payload)
   if state.pop('lose_close_response',False):fail('lost status response')
  emit(row)
 if parts[2]=='sub_issues':
  if method=='POST':
   child=payload['sub_issue_id']
   if any(child in other.get('children',[]) and other['id']!=row['id'] for other in issues.values()):fail('conflicting parent')
   if child not in row['children']:row['children'].append(child)
   if state.pop('lose_edge_response',False):fail('lost edge response')
  emit(paginated([other for other in issues.values() if other['id'] in row.get('children',[])]))
 if parts[2:]==['dependencies','blocked_by']:
  if method=='POST' and payload['issue_id'] not in row['blocked']:row['blocked'].append(payload['issue_id'])
  emit(paginated([other for other in issues.values() if other['id'] in row.get('blocked',[])]))
 if parts[2]=='comments':
  comments=row.setdefault('comments_data',[])
  if method=='POST':
   comments.append({'id':len(comments)+1,'body':payload['body']})
   if state.pop('lose_comment_response',False):fail('lost comment response')
   emit(comments[-1])
  emit(paginated(comments))
fail('unsupported fixture request')
