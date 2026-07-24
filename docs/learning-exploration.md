---
id: learning-exploration
title: Learning and exploration boundaries
description: Understand the explicit boundary between offline adaptation and hypothesis-driven discovery.
---

# Learning and exploration boundaries

Wuwe implements Learning & Adaptation and Exploration & Discovery as separate modules:

- [Learning & Adaptation](learning-adaptation.md) turns recorded experience and rewards into versioned candidates, then gates promotion through evaluation, approval, activation, and rollback.
- [Exploration & Discovery](exploration-discovery.md) generates hypotheses, plans controlled experiments, reviews evidence, and persists conclusions.

Neither module silently rewrites an Agent or updates model weights. Exploration does not activate a learned artifact. Learning does not start open-ended experiments. The optional `exploration_adapter.hpp` bridge only converts recorded experiment evidence into explicit Learning experiences; reward creation still requires an application-supplied mapper.
