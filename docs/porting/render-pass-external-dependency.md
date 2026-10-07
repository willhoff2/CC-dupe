# Render pass external dependency: the depth attachment's WRITE_AFTER_WRITE

Found by synchronization validation (`VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`) while measuring the
non-blocking Present (`perf/text-render-no-flush`, dropped; see that branch's
`docs/porting/present-pipelining.md`). The defect and its fix are independent of that work.

## Defect (MEASURED, M1 Pro, MoltenVK 1.4.2, validation layers 1.4.357)

A pass break keeps an attachment's tracked layout, so `Transition_Surface` records no barrier between
one render pass's store and the next pass's transition out of `VK_IMAGE_LAYOUT_UNDEFINED`. The render
pass declared no subpass dependency, and the implicit `VK_SUBPASS_EXTERNAL` dependency starts at
`TOP_OF_PIPE`, which orders nothing. Synchronization validation reports it on the depth attachment:

```
vkCmdBeginRenderPass(): WRITE_AFTER_WRITE hazard detected. vkCmdBeginRenderPass performs image layout
transition in subpass 0 ... on attachment 1 ... oldLayout VK_IMAGE_LAYOUT_UNDEFINED ...
No sufficient synchronization is present to ensure that a layout transition does not conflict with a
prior write (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) at VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT.
```

Plain validation (the mode CI ran) reports nothing, which is why it was not seen before.

## Fix

`Get_Or_Create_Render_Pass` declares one `VK_SUBPASS_EXTERNAL -> 0` dependency over the colour and
depth attachment stages and their writes, so the new pass's layout transition is ordered after the
previous pass's store.

## Evidence (MEASURED, same machine, `spikes/renderer` Release build)

| build | `zh-resource-lock-tests` with sync validation | `zh-fixedfunc-tests` with sync validation |
|---|---|---|
| `main` (`6a593e409`) | 3 messages, exit 1 | 6 messages, exit 1 |
| this fix | 0 messages, exit 0 | 0 messages, exit 0 |

Both binaries exit non-zero on any validation message, so the macOS spike job now runs each of them a
second time with `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true` as the regression gate. Without sync
validation both stay at 0 messages and every case result is unchanged.

## Not measured

- Whether the hazard ever produced a visible artefact. The existing pixel assertions passed before the
  fix, so on MoltenVK it is a correctness gap the driver happened to tolerate (INFERRED).
- Sync validation on Linux/lavapipe: the Linux job is not changed. Add it there once a lavapipe run
  shows it clean.
