# Adreno740

Native XNU Adreno driver. Owns GMU startup, the CP ring, BOs, native 4 KiB SMMU contexts, fences and cache maintenance.

Current state on sheng (Adreno 740): GPU commands and the scanout copy run on hardware. The same driver carries an Adreno 732 (SM7675, `gen71100` firmware) profile for uke; its SoC-specific values are selected from the GPU node's `qcom,chipid` and `reg-names`, but the uke path has not run on a device yet. The rootstockos `asteroids-a810` branch adds the Adreno 810 (SM7635, `gen80300` firmware) GMU and GPU-context path for the Nothing Phone (3a).

Build and interface details: [documentation](https://unixnu.com/drivers/adreno740). Component sources and dependencies: [catalog](https://unixnu.com/reference/components).
