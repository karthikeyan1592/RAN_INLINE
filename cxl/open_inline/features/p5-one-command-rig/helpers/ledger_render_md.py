#!/usr/bin/env python3
"""ledger_render_md.py -- deterministic Markdown render of oi-p5-ledger/1 (D4: JSON is
authoritative, this is a pure render, never a second source of truth)."""
import argparse
import json
import sys

STATUS_ICON = {
    "PASS": "PASS", "FAIL": "FAIL", "ERROR": "ERROR", "BLOCKED": "BLOCKED", "TIMEOUT": "TIMEOUT",
}


def render(ledger):
    lines = []
    lines.append(f"# p5 simtest ledger — `{ledger['run_id']}`")
    lines.append("")
    lines.append(f"- schema: `{ledger['schema']}`")
    lines.append(f"- tier: `{ledger['tier']}`")
    lines.append(f"- host: `{ledger['host'].get('kind')}`")
    lines.append(f"- started: {ledger['started_utc']}  finished: {ledger['finished_utc']}")
    lines.append(f"- pins_digest: `{ledger['pins_digest']}`")
    lines.append(f"- rigcfg_digest: `{ledger['rigcfg_digest']}`")
    lines.append(f"- **overall: {ledger['overall']}**")
    lines.append("")
    for phase in ledger["phases"]:
        if not phase["discovered"]:
            reason = phase.get("validation_error", phase.get("not_discovered_reason", "no gates/suite.yml found"))
            lines.append(f"## {phase['phase']} ({phase['feature']}) — NOT_DISCOVERED")
            lines.append(f"- reason: {reason}")
            lines.append("")
            continue
        lines.append(f"## {phase['phase']} ({phase['feature']})")
        lines.append("")
        lines.append("| gate | type | status | exit_code | duration_s |")
        lines.append("|---|---|---|---|---|")
        for g in phase["gates"]:
            lines.append(
                f"| {g['id']} | {g['type']} | {STATUS_ICON.get(g['status'], g['status'])} | "
                f"{g['exit_code']} | {g['duration_s']:.3f} |"
            )
        lines.append("")
    lines.append("## Honesty notes")
    for note in ledger["honesty_notes"]:
        lines.append(f"- {note}")
    lines.append("")
    lines.append(f"performance_claims: `{json.dumps(ledger['performance_claims'])}` "
                  "(always empty at SIM tier)")
    lines.append("")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ledger_json")
    ap.add_argument("-o", "--out")
    args = ap.parse_args()

    with open(args.ledger_json) as f:
        ledger = json.load(f)

    md = render(ledger)
    if args.out:
        with open(args.out, "w") as f:
            f.write(md)
        print(args.out)
    else:
        print(md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
