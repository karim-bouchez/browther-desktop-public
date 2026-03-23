#!/usr/bin/env python3
# Copyright (c) 2026 The Brave Authors. All rights reserved.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.
"""Run Claude-driven PR review in CI (Docker/GitHub Actions).

Fetches PR context and diff via gh, sends to Anthropic API with the **review-prs**
skill instructions (.claude/skills/review-prs/SKILL.md), and posts the result as
a GitHub review: one summary body plus multiple inline comments (when the model
provides them).

This is a single-pass CI analogue of `/review-prs … auto reviewer-priority`: it
does not run prepare-review.py, subagents, or collect-results.py from that skill.

Usage:
  python3 pr-review-claude.py <pr_number> [repo]
  repo defaults to brave/brave-core.

Credentials (in order):
  1. If stdin is not a TTY, read two lines: GitHub token, then Anthropic API key
     (recommended for Docker so secrets are not passed via -e / process listings).
  2. Otherwise use environment variables:
       GH_TOKEN          - GitHub token (pull request read + comment write)
       ANTHROPIC_API_KEY - Anthropic API key for Claude
"""

import argparse
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path

try:
    import anthropic
except ImportError:
    print("anthropic package required; run: pip install anthropic", file=sys.stderr)
    sys.exit(1)

REPO_DEFAULT = "brave/brave-core"


def _resolve_credentials() -> tuple[str, str]:
    """Prefer stdin (two lines: GH token, Anthropic key) when piped; else env vars."""
    if not sys.stdin.isatty():
        line1 = sys.stdin.readline()
        line2 = sys.stdin.readline()
        if line1 and line2:
            return (
                line1.rstrip("\r\n"),
                line2.rstrip("\r\n"),
            )
    gh = os.environ.get("GH_TOKEN", "")
    ak = os.environ.get("ANTHROPIC_API_KEY", "")
    return gh, ak


def _skill_path() -> Path:
    """review-prs skill: baked into the CI image (trusted), or next to the repo in dev."""
    bundled = Path("/opt/pr-review-claude/skills/review-prs/SKILL.md")
    if bundled.exists():
        return bundled
    # Local dev: script at .github/workflows/pr-review-claude.py → repo root is .parent.parent.parent
    repo_root = Path(__file__).resolve().parent.parent.parent
    return repo_root / ".claude" / "skills" / "review-prs" / "SKILL.md"


REVIEW_DISCLAIMER = (
    "I generated this review about the changes, sharing here. "
    "It should be used for informational purposes only and not as proof of review.\n\n"
)


def _finalize_review_body(body: str) -> str:
    """Ensure exactly one canonical disclaimer prefix (single place; not also in the model prompt)."""
    if not (body or "").strip():
        return ""
    if body.startswith(REVIEW_DISCLAIMER):
        return body
    return REVIEW_DISCLAIMER + body


def _body_for_posting(body: str, *, has_inline_comments: bool) -> str:
    """Build the final body we send to GitHub, always keeping disclaimer semantics."""
    finalized = _finalize_review_body(body)
    if finalized:
        return finalized
    if has_inline_comments:
        return REVIEW_DISCLAIMER + "See inline comments."
    return "No review content."


def _cmd_error_detail(stderr: str | None, stdout: str | None) -> str:
    parts = []
    serr = (stderr or "").strip()
    sout = (stdout or "").strip()
    if serr:
        parts.append(f"stderr: {serr}")
    if sout:
        parts.append(f"stdout: {sout}")
    return "; ".join(parts) if parts else "(no stderr or stdout captured)"


def _gh_env(gh_token: str):
    # gh reads GH_TOKEN from the child env; we keep token in locals only (stdin path never sets os.environ).
    return {**os.environ, "GH_TOKEN": gh_token}


def _run_gh_command(
    cmd: list[str],
    *,
    gh_token: str,
    stdin: str | None = None,
    capture: bool = True,
):
    """Run gh; always capture stderr/stdout so failures are diagnosable in CI."""
    try:
        result = subprocess.run(
            cmd,
            input=stdin,
            text=True,
            env=_gh_env(gh_token),
            capture_output=True,
            check=True,
        )
    except subprocess.CalledProcessError as e:
        cmd_s = " ".join(shlex.quote(str(c)) for c in e.cmd)
        raise RuntimeError(
            f"gh failed (exit {e.returncode}): {cmd_s}\n{_cmd_error_detail(e.stderr, e.stdout)}"
        ) from e
    return result.stdout.strip() if capture else None


def run_gh(*args, repo: str, gh_token: str, capture=True):
    cmd = ["gh", *args, "-R", repo]
    return _run_gh_command(cmd, gh_token=gh_token, capture=capture)


def gather_pr_context(pr_number: str, repo: str, *, gh_token: str) -> dict:
    """Fetch PR title, body, and diff via gh."""
    out = run_gh(
        "pr", "view", pr_number, "--json", "title,body,author",
        repo=repo,
        gh_token=gh_token,
    )
    meta = json.loads(out)
    diff = run_gh("pr", "diff", pr_number, repo=repo, gh_token=gh_token)
    return {
        "title": meta.get("title", ""),
        "body": meta.get("body") or "",
        "author": (meta.get("author") or {}).get("login", ""),
        "diff": diff,
    }


def load_skill_content() -> str:
    """Load the review-prs skill markdown (for system prompt)."""
    path = _skill_path()
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def run_review(pr_number: str, repo: str, *, gh_token: str, api_key: str) -> str:
    ctx = gather_pr_context(pr_number, repo, gh_token=gh_token)
    skill_content = load_skill_content()

    system = (
        "You are performing a PR best-practices review for brave-core, aligned with the **review-prs** skill below "
        "(same scope as `/review-prs … auto reviewer-priority`: only violations of existing documented best practices; "
        "NEVER create or modify best-practice rules or docs during this run).\n\n"
        "**CI constraints:** You cannot run bash, prepare-review.py, collect-results.py, subagents, or read other files. "
        "Use ONLY the PR title, body, and diff in the user message. CI will post the review to GitHub (you must not claim to post yourself).\n\n"
        "Output a single JSON object with two keys: 'body' and 'comments'. "
        "'body' (string): full review in markdown (summary, best-practice findings, verdict if appropriate). "
        "Do not include a review disclaimer in 'body'; CI adds the standard disclaimer when posting. "
        "'comments' (array): for each issue tied to a specific line, one object with 'path' (as in the diff), "
        "'line' (line number in the new file), 'body' (1-3 sentences). Put non-line-specific issues in 'body' only. "
        "Output only the JSON object, no surrounding text or markdown code fence.\n\n"
        "---\n"
        + (skill_content if skill_content else "Review the diff against documented best practices only.")
    )

    user_content = (
        f"Invoke as for review-prs: single PR #{pr_number}, open, auto, reviewer-priority (CI headless).\n\n"
        f"**Repo**: {repo}\n**PR**: [#{pr_number}](https://github.com/{repo}/pull/{pr_number})\n"
        f"**Title**: {ctx['title']}\n**Author**: @{ctx['author']}\n\n"
        f"**PR body:**\n```\n{ctx['body'][:15000]}\n```\n\n"
        f"**Diff:**\n```diff\n{ctx['diff'][:180000]}\n```\n\n"
        "Produce the review as a JSON object with 'body' and 'comments' (array of {path, line, body}) for inline comments."
    )

    client = anthropic.Anthropic(api_key=api_key)
    message = client.messages.create(
        model="claude-sonnet-4-20250514",
        max_tokens=8192,
        system=system,
        messages=[{"role": "user", "content": user_content}],
    )

    text = ""
    for block in message.content:
        if hasattr(block, "text"):
            text += block.text
    return text.strip()


def _strip_optional_code_fence(text: str) -> str:
    """If the model wrapped content in markdown fences, extract the inner part.

    Tolerant of a missing closing fence (use find, not index; never raise ValueError).
    """
    t = text.strip()
    if "```json" in t:
        key = "```json"
        start = t.find(key)
        if start == -1:
            return t
        i = start + len(key)
        end = t.find("```", i)
        if end != -1:
            return t[i:end].strip()
        return t[i:].strip()
    if "```" in t:
        start = t.find("```")
        if start == -1:
            return t
        i = start + 3
        end = t.find("```", i)
        if end != -1:
            return t[i:end].strip()
        return t[i:].strip()
    return t


def _parse_review_response(raw: str) -> tuple[str, list[dict]]:
    """Parse Claude response into body and comments. Falls back to raw as body if not JSON."""
    raw = _strip_optional_code_fence(raw.strip())
    try:
        data = json.loads(raw)
        body = data.get("body") or ""
        comments = data.get("comments") or []
        if not isinstance(comments, list):
            comments = []
        # Normalize: each comment needs path, line (int), body; add side for API
        out = []
        for c in comments:
            if isinstance(c, dict) and c.get("path") and c.get("line") is not None and c.get("body"):
                out.append({
                    "path": str(c["path"]).strip(),
                    "line": int(c["line"]),
                    "side": "RIGHT",
                    "body": str(c["body"]).strip()[:65535],
                })
        body = _finalize_review_body(body)
        return body, out
    except (json.JSONDecodeError, ValueError, TypeError):
        return _finalize_review_body(raw), []


def post_review(
    pr_number: str,
    repo: str,
    body: str,
    comments: list[dict],
    *,
    gh_token: str,
) -> None:
    """Post as a GitHub review (body + inline comments) or a single PR comment if no inlines."""
    posted_body = _body_for_posting(body, has_inline_comments=bool(comments))
    if comments:
        payload = {
            "event": "COMMENT",
            "body": posted_body,
            "comments": comments,
        }
        cmd = [
            "gh", "api", f"repos/{repo}/pulls/{pr_number}/reviews",
            "--method", "POST", "-R", repo, "--input", "-",
        ]
        _run_gh_command(
            cmd,
            gh_token=gh_token,
            stdin=json.dumps(payload),
            capture=False,
        )
    else:
        run_gh(
            "pr",
            "comment",
            pr_number,
            "-b",
            posted_body,
            repo=repo,
            gh_token=gh_token,
            capture=False,
        )


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run Claude-driven PR review in CI.",
    )
    parser.add_argument("pr_number", help="Pull request number")
    parser.add_argument("repo", nargs="?", default=REPO_DEFAULT, help="GitHub repo (owner/name)")
    return parser.parse_args(argv)


def Main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    pr_number = args.pr_number.strip()
    repo = args.repo.strip()

    gh_token, api_key = _resolve_credentials()
    if not gh_token:
        print(
            "GitHub token missing (stdin line 1 or GH_TOKEN)",
            file=sys.stderr,
        )
        return 1
    if not api_key:
        print(
            "Anthropic API key missing (stdin line 2 or ANTHROPIC_API_KEY)",
            file=sys.stderr,
        )
        return 1

    try:
        raw = run_review(pr_number, repo, gh_token=gh_token, api_key=api_key)
        body, comments = _parse_review_response(raw)
        post_review(pr_number, repo, body, comments, gh_token=gh_token)
        if comments:
            print(f"Posted review with {len(comments)} inline comment(s) on PR #{pr_number}")
        else:
            print("Posted review comment on PR #" + pr_number)
        return 0
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(Main())
