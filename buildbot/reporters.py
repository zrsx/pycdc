"""GitHub commit and pull-request status reporting."""

from buildbot.plugins import reporters, util

from config import GITHUB_TOKEN


def create_reporters():
    """Publish pending/final status and concise build descriptions."""
    return [
        reporters.GitHubStatusPush(
            token=GITHUB_TOKEN,
            context=util.Interpolate("buildbot/%(prop:buildername)s"),
            verbose=True,
        )
    ]
