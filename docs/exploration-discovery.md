---
id: exploration-discovery
title: Exploration and discovery
description: Run bounded hypothesis generation, controlled experiments, evidence review, and explicit evidence retention.
---

# Exploration and discovery

The Exploration module coordinates a bounded scientific-style loop without granting an Agent open-ended authority:

1. generate hypotheses;
2. design experiments;
3. classify each experiment as read-only or effectful;
4. evaluate declared capabilities and obtain approval where required;
5. execute approved work with bounded concurrency, cancellation, and timeout;
6. review evidence and record a supported, refuted, inconclusive, blocked, or failed verdict;
7. persist the complete hypothesis, experiment, approval, evidence, and conclusion record.

```cpp
exploration::exploration_runner runner({
  .generate = generate_hypotheses,
  .design = design_experiments,
  .execute = run_controlled_experiment,
  .review = review_evidence,
  .store = &exploration_store,
  .approvals = &approval_service,
});

const auto result = runner.run({
  .objective = "find the cause of retrieval latency",
}, {
  .policy = {
    .max_hypotheses = 4,
    .max_experiments = 8,
    .max_concurrency = 2,
    .timeout = std::chrono::seconds(30),
  },
});
```

## Safety boundary

Effectful experiments are disabled by default. Enabling `allow_effectful_experiments` does not bypass approval: approval for effectful work and declared capabilities remains enabled by default. Wuwe adds the generic `exploration.execute` capability; applications should also declare concrete filesystem, network, process, environment, or secret capabilities.

The executor callback is application code. Marking an experiment `read_only` is a policy assertion, not a sandbox. Use the Execution module and enforce capability policy when an experiment starts a process, writes files, changes configuration, accesses a network, or touches secrets.

## Evidence and review

`experiment_evidence` retains structured observations, artifact references, a summary, errors, elapsed time, and metadata. Review operates on the collected evidence for one hypothesis and produces a verdict, confidence, conclusion, and supporting or counter evidence.

A confidence threshold can downgrade a nominal supported/refuted assessment to inconclusive. Blocked experiments and execution failures remain distinguishable from a scientific result that is merely inconclusive.

## Explicit Learning bridge

Exploration never changes an active Prompt, strategy, or policy. To reuse evidence in an offline Learning workflow, include `wuwe/agent/learning/exploration_adapter.hpp` explicitly:

```cpp
const auto imported = learning::persist_exploration_experiences(
  exploration_result.record,
  { .target = "retrieval.configuration" },
  experience_store);
```

Each imported record retains the objective, hypothesis, experiment trajectory, evidence summary, verdict, status, and source-run provenance. No Reward is created by default. Pass both a Reward Store and an `exploration_reward_mapper` only when the application has an explicit domain rule for translating evidence into a signed reward.

## Cancellation and persistence

Generator, designer, executor, and reviewer callbacks receive a stop token and effective deadline. Work that ignores cancellation may be returned as detached while it finishes. Store only owned values in callbacks and make effectful operations idempotent.

`in_memory_exploration_store` is a thread-safe reference implementation. Production implementations should add tenancy, retention, artifact storage, audit controls, and any required transaction boundary.

See `examples/src/exploration_discovery_example.cpp` for a read-only experiment and explicit evidence import.
