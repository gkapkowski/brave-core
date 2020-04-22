---
name: Rewards PR
about: 'Template for creating Rewards PR'
title: ''
labels: 'feature/rewards'
assignees: ''

---

<!-- Add brave-browser issue bellow that this PR will resolve -->
Resolves 

## Submitter Checklist:

- [ ] Submitted a [ticket](https://github.com/brave/brave-browser/issues) for my issue if one did not already exist.
- [ ] Used Github [auto-closing keywords](https://help.github.com/articles/closing-issues-via-commit-messages/) in the commit message.
- [ ] Moved an issue to [Rewards Global project](https://github.com/orgs/brave/projects/18) (move it to correct column based on the progress)
- [ ] Added/updated tests for this change (for new code or code which already has tests).
- Verified that these changes build without errors on
  - [ ] Android
  - [ ] iOS
  - [ ] Linux
  - [ ] macOS
  - [ ] Windows
- [ ] Tagged reviewers and labelled the pull request as needed.
- [ ] Added appropriate QA label (QA/Yes or QA/No) on the issue/s that this PR closes
- [ ] Left a note in Slack channel (#rewards-dev) about PR ready for a review
- [ ] Verified that CI passed (or left a comment if it failed on a known issue)
- Check if needed
  - [ ] Requested a security/privacy review. Read when needed [here](https://github.com/brave/handbook/blob/master/development/security.md#security-reviews).
  - [ ] Uploaded DB version, if new version was created in the PR, into this [folder](https://github.com/brave/rewards/tree/master/database).

## Test Plan:


## Reviewer Checklist:

- New files have MPL-2.0 license header with correct year.
- Adequate test coverage exists to prevent regressions 
- Verify test plan is specified in PR
- Verify that CI passed

## After-merge Checklist:

- [ ] Set milestone for PR and the issue, so that it matches branch that PR was merged into.
- [ ] All relevant documentation has been updated.
