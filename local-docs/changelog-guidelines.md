# Changelog writing guideline

Write each changelog entry as one short, clear sentence stating what changed and
the specific problem it fixes or benefit it provides. Prefix the entry with a
topic tag, using the associated epic's name where applicable; omit the tag when
no meaningful topic fits. Describe delivered behavior and avoid unsupported claims.

Suggested pattern: **[Topic] [Changed what] so [practical benefit].**

Use plain language and name the affected behavior or component. Describe the
delivered behavior rather than simply rephrasing the issue title, and keep the
benefit within the change's actual scope. Topic tags are useful grouping, not
mandatory invented categories; use concise epic-derived names such as
**[Observability]** and **[Harness]**. Include a link to the associated issue for
traceability, using its full `https://github.com/robinwassen/Fossilize/issues/NUMBER`
URL.

For example:

- **[Observability]** Fixed file write helpers to report write, flush and close errors so callers can detect incomplete output. ([#12](https://github.com/robinwassen/Fossilize/issues/12))

## Maintaining the record

Add or revise the relevant entry in root [CHANGELOG.md](../CHANGELOG.md) in the
same implementation PR as the change. Revise its wording whenever implementation
or review changes the final scope. Independent review checks the entry against
the actual candidate, including its accuracy, practical benefit, topic, brevity
and issue link; final review covers the combined changelog too.

Use an Unreleased section for changes without an assigned release; an entry in
an open implementation PR describes its reviewed candidate scope, not confirmed
merged delivery. The actual merged PR establishes delivery; no follow-up status
edit or release date is required merely because it merged. Do not present
unsupported, failed or incomplete work as completed, or turn desired properties
and unrun checks into verified claims. Historical entries describe the behavior
delivered at that time; identify later supersession when needed to avoid implying
that an obsolete mechanism remains current.

Use understandable historical ordering or topic grouping without inventing release
versions or release dates. The changelog is a historical change record, not a
second backlog: future work, requirements, dependencies and task status stay in
[GitHub Issues](https://github.com/robinwassen/Fossilize/issues) under the
[issue workflow](issue-workflow.md).
