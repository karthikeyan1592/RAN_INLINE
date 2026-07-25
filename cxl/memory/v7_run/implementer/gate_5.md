## Spec

Gate 5 — GCP instance deleted

Criterion:
- (a) `gcloud compute instances list --project=cxl-systems-lab-26` shows cxl-systems-lab absent

Note: run from a machine with gcloud authenticated to project cxl-systems-lab-26
(zone: asia-south2-a).

## Commands

```bash
# Delete instance:
gcloud compute instances delete cxl-systems-lab \
  --project=cxl-systems-lab-26 \
  --zone=asia-south2-a \
  --quiet

# Confirm deletion:
gcloud compute instances list --project=cxl-systems-lab-26
```

Expected output (no instances listed):
```
Listed 0 items.
```

## Raw evidence

<!-- PASTE VERBATIM TERMINAL OUTPUT BELOW — must show instances list result -->

```

```

<!-- END RAW EVIDENCE -->

## Self-verdict

| Criterion | Status | Evidence |
|-----------|--------|----------|
| (a) instance absent from gcloud list | PENDING | |

**Overall: PENDING**
