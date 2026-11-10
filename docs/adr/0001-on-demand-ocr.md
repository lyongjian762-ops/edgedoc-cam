# ADR 0001: Run OCR on selected still images

Status: accepted for the first release. Date: 2026-11-09.

## Context

The camera can produce frames faster than the CPU OCR model can process them. The historical smoke request is not a controlled benchmark. Processing every frame risks a growing backlog and unresponsive preview.

## Decision

Keep preview continuous and submit OCR only for an explicit or quality-gated still image. Own the snapshot before returning its V4L2 buffer. Use a bounded asynchronous job queue and a CPU reference path; evaluate NPU support independently.

## Consequences

Preview and OCR have separate lifecycles and metrics. Buffer ownership, queue limits and error handling need tests. Automatic capture and NPU acceleration remain experiments until verified.
