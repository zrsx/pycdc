"""Git and GitHub pull-request change sources."""

from buildbot.plugins import changes

from config import (
    CHANGE_PASSWORD,
    CHANGE_USER,
    GITHUB_OWNER,
    GITHUB_REPOSITORY,
    GITHUB_TOKEN,
    GIT_POLL_INTERVAL,
    GIT_URL,
    PR_POLL_INTERVAL,
    PROJECT_NAME,
)


def create_change_sources():
    """Use webhooks primarily and pollers as loss/restart recovery.

    A PBChangeSource is also exposed so an operator (or the GitHub Actions
    workflow) can inject a change over the master PB port with
    ``buildbot sendchange`` without depending on the GitHub webhook.
    """
    return [
        changes.PBChangeSource(user=CHANGE_USER, passwd=CHANGE_PASSWORD),
        changes.GitPoller(
            repourl=GIT_URL,
            branches=True,
            pollInterval=GIT_POLL_INTERVAL,
            pollRandomDelayMin=5,
            pollRandomDelayMax=20,
            project=PROJECT_NAME,
        ),
        changes.GitHubPullrequestPoller(
            owner=GITHUB_OWNER,
            repo=GITHUB_REPOSITORY,
            token=GITHUB_TOKEN,
            pollInterval=PR_POLL_INTERVAL,
            project=PROJECT_NAME,
        ),
    ]
