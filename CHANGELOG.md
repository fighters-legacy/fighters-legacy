# Changelog

All notable changes to this project will be documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- **audio**: Listener velocity is now wired from the player entity to OpenAL; Doppler pitch shift is audible when flying past positional audio sources at high speed (closes #167)
- **renderer**: Particle emitters with `spawnRate < 60/s` now produce correct output at 60 fps; fractional remainder is carried across frames instead of being truncated to zero each frame (closes #263)

### Changed

- **renderer**: `rain` and `storm_rain` preset spawn rates reduced from 600/s and 1200/s to 100/s and 200/s respectively; the previous values compensated for the truncation bug and also exceeded `kMaxParticles=8192` at 9 emitters

## [0.2.2] - 2026-05-27

### Changed

- Fix artifact glob and add attestations permission

## [0.2.1] - 2026-05-27

### Added

- Add fallback release notes when cliff output is empty

## [0.2.0] - 2026-05-26

### Added

- Add CMake skeleton with subdirs and dependency management (#68)
- **platform**: Define HAL interface headers (closes #14) (#76)
- **content**: Implement content pack and mod system (#78)
- **renderer**: Implement Vulkan + MoltenVK renderer backend (#79)
- **input**: Add SDL3 input backend, engine binding/axis layer, and input_test tool (#84)
- **audio**: Implement OpenAL Soft backend (closes #18) (#85)
- **network**: Implement ENet backend, fl-server binary, and network tests (#93)
- **engine**: Add i18n infrastructure (closes #20) (#95)
- **engine**: Add first-run detection and user config persistence (closes #22) (#99)
- **engine,game**: Add crash reporting, FileLogger, and fighters-legacy stub (#101)
- **engine**: Add graphics and audio mix settings to user config (#105)
- **engine,platform,game**: Boot without content pack; sandbox inspector on first run (#113)

### Changed

- Add prior-art simulator landscape and FDM RFC reference (#104)
- Remove fa-content from roadmap; pivot to fl-base-pack (#111)
- Add technology reference index and fix reuse lint (#112)
- **roadmap**: Update phase 1 acceptance criteria (#114)
## [0.0.1] - 2026-05-22

### Changed

- Add notes about fa-content repo (#1)
- Slim README to project card; extract roadmap to docs and GitHub issues (#59)

### Fixed

- Wrap SPDX example snippets with REUSE-IgnoreStart markers (#11)
