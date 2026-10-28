"""Fork-bound GitHub task data, read through gh; never execute issue content."""
import hashlib
import json
import re
import subprocess
from urllib.parse import urlsplit

REPOSITORY = 'robinwassen/Fossilize'


class IssueError(ValueError):
    pass


def need(ok, message):
    if not ok:
        raise IssueError(message)


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


class GitHub:
    def __init__(self, repository=REPOSITORY, executable='gh', timeout=60):
        need(repository == REPOSITORY, 'explicit fork robinwassen/Fossilize required')
        self.repository, self.executable, self.timeout = repository, executable, timeout

    def call(self, args, payload=None):
        try:
            result = subprocess.run([self.executable, *args], input=None if payload is None else json.dumps(payload),
                                    text=True, capture_output=True, timeout=self.timeout)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise IssueError('gh unavailable/timeout; reconcile service before retry') from error
        need(result.returncode == 0, 'gh request failed; authentication/network/API result is not absence')
        try:
            return json.loads(result.stdout) if result.stdout.strip() else None
        except ValueError as error:
            raise IssueError('malformed gh response; reconcile service before retry') from error

    def api(self, suffix, method='GET', payload=None, paginate=False):
        need(not suffix.startswith('/') and '..' not in suffix, 'invalid API resource')
        args = ['api', '--hostname', 'github.com', '--method', method,
                'repos/' + self.repository + '/' + suffix]
        if paginate:
            args += ['--paginate', '--slurp']
        if payload is not None:
            args += ['--input', '-']
        result = self.call(args, payload)
        if paginate:
            need(isinstance(result, list) and all(isinstance(page, list) for page in result), 'incomplete paginated response')
            return [row for page in result for row in page]
        return result

    def number(self, value):
        if isinstance(value, int) or (isinstance(value, str) and value.isdigit()):
            number = int(value)
        else:
            parsed = urlsplit(str(value))
            match = re.fullmatch('/' + re.escape(self.repository) + r'/issues/([1-9][0-9]*)', parsed.path)
            need(parsed.scheme == 'https' and parsed.hostname == 'github.com' and not parsed.query and not parsed.fragment and match,
                 'issue URL must identify the explicit fork and an issue')
            number = int(match[1])
        need(number > 0, 'invalid issue number')
        return number

    def issues(self):
        return [row for row in self.api('issues?state=all&per_page=100', paginate=True) if 'pull_request' not in row]

    def comments(self, number):
        return self.api(f'issues/{self.number(number)}/comments?per_page=100', paginate=True)

    def issue(self, value):
        number = self.number(value)
        row = self.api(f'issues/{number}')
        need(isinstance(row, dict) and 'pull_request' not in row and row.get('number') == number, 'root is missing or is a pull request')
        url = f'https://github.com/{self.repository}/issues/{number}'
        need(row.get('html_url') == url and row.get('id') and row.get('node_id'), 'issue repository/server identity mismatch')
        children = self.api(f'issues/{number}/sub_issues?per_page=100', paginate=True)
        blocked = self.api(f'issues/{number}/dependencies/blocked_by?per_page=100', paginate=True)
        # Parent endpoint returns 404 for roots. GraphQL field is nullable and fails
        # loudly for transport/auth errors, unlike interpreting any 404 as no parent.
        parent = self.call(['api', '--hostname', 'github.com', 'graphql', '-f',
            'query=query($owner:String!,$name:String!,$number:Int!){repository(owner:$owner,name:$name){issue(number:$number){parent{url}}}}',
            '-f', 'owner=' + self.repository.split('/')[0], '-f', 'name=' + self.repository.split('/')[1], '-F', 'number=' + str(number)])
        need(isinstance(parent, dict) and not parent.get('errors'), 'parent lookup failed')
        try:
            parent = parent['data']['repository']['issue']['parent']
        except (KeyError, TypeError) as error:
            raise IssueError('incomplete parent lookup') from error
        def identities(rows):
            need(all(isinstance(item, dict) and item.get('html_url') and item.get('id') for item in rows), 'incomplete relationship read')
            for item in rows:
                self.number(item['html_url'])
            return sorted(item['html_url'] for item in rows)
        return {'url': url, 'id': row['id'], 'node_id': row['node_id'], 'repository': self.repository,
                'number': number, 'title': row['title'], 'body': row.get('body') or '',
                'state': row['state'], 'state_reason': row.get('state_reason'),
                'parent': parent['url'] if parent else None, 'children': identities(children), 'blocked_by': identities(blocked)}

    def snapshot(self, root, deliverables=None):
        root = f'https://github.com/{self.repository}/issues/{self.number(root)}'
        nodes, visiting = {}, set()
        def visit(url):
            need(url not in visiting, 'cyclic prerequisite graph')
            if url in nodes:
                return
            visiting.add(url)
            row = self.issue(url)
            nodes[url] = row
            for dependency in row['blocked_by']:
                visit(dependency)
            visiting.remove(url)
        deliverables=sorted(set([root]+[f'https://github.com/{self.repository}/issues/{self.number(value)}' for value in (deliverables or [])]))
        for issue in deliverables:visit(issue)
        value = {'root': root, 'deliverables':deliverables, 'nodes': nodes}
        value['digest'] = digest(value)
        return value

    @staticmethod
    def ready(snapshot):
        return all(row['state'] == 'closed' and row['state_reason'] == 'completed'
                   for url, row in snapshot['nodes'].items() if url not in snapshot['deliverables'])

    def unchanged(self, snapshot):
        current = self.snapshot(snapshot['root'],snapshot.get('deliverables'))
        need(current['digest'] == snapshot['digest'], 'material issue input changed; reconcile snapshot and renew all verification')
        return current
