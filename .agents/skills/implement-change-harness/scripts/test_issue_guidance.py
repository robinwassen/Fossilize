"""G01 source authority and package-guidance regression checks."""
from pathlib import Path
import re
import unittest
from harness_state import git


class IssueGuidanceTests(unittest.TestCase):
    def test_G01_source_has_policy_without_task_registry_or_mirror(self):
        repo=Path(git(Path.cwd(),'rev-parse','--show-toplevel').decode().strip())
        for path in ('local-docs/backlog-keys.md','local-docs/reviews/harness-backlog.md','local-docs/reviews/github-issues-migration.md'):
            self.assertFalse((repo/path).exists(),path)
        agents=(repo/'AGENTS.md').read_text()
        self.assertIn('local-docs/issue-workflow.md',agents)
        self.assertIn('robinwassen/Fossilize/issues',agents)
        policy=(repo/'local-docs/issue-workflow.md').read_text()
        self.assertIn('supersedes the former requirement',policy)
        self.assertIn('work unit',policy)
        guidance=[repo/'AGENTS.md',*(repo/'local-docs').rglob('*.md'),*(repo/'.agents/skills/implement-change-harness').rglob('*.md')]
        for path in guidance:
            text=path.read_text()
            self.assertIsNone(re.search(r'(?m)^(?:#{1,6}\s+|\d+\.\s+\*\*)FOS-\d{6,}\s+[—–-]',text),str(path))
            self.assertNotIn('backlog-keys.md',text,str(path))
            for target in re.findall(r'\[[^\]]*\]\(([^)]+)\)',text):
                if '://' in target or target.startswith(('#','mailto:')):continue
                destination=(path.parent/target.split('#')[0]).resolve()
                self.assertTrue(destination.exists(),str(path)+': '+target)
        skill=(repo/'.agents/skills/implement-change-harness/SKILL.md').read_text()
        self.assertNotIn('subtask',skill)
        self.assertNotIn('Done markers',skill)
        self.assertIn('issue-evidence',skill)
