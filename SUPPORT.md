# Support

Status: publicly available experimental source. There is no supported binary release or promised response time.

Direct USB reception is implemented for **HackRF One and RTL-SDR**, with experimental **one/two-board RAK5146 USB/LBT** support. Check [receiver and platform compatibility](docs/operations/hardware-compatibility.md) for platform limits and what has been tested. RAK uses [sampled RSSI scans and configured packet reception](docs/operations/rak5146.md), not an SDR waterfall or continuous occupancy measurement.

For community support, setup questions and RF survey discussion, join the [OVMesh Discord](https://discord.gg/kwKhFamfaU). Use GitHub issues for tracked non-sensitive bug reports and feature requests.

Begin with the [user guide](docs/user-guide.md), [build guide](docs/operations/deployment.md), [support playbook](docs/operations/support-playbook.md) and [known limits](docs/engineering/implementation-status.md). Search [GitHub issues](https://github.com/OVMeshRF/OVMeshDRpp/issues) for an existing report, or [open an issue](https://github.com/OVMeshRF/OVMeshDRpp/issues/new/choose) for a non-sensitive bug, feature request or support question.

Support reports should include application/OS/driver versions, device model, sanitized configuration, symptom timing, relevant receiver-health counters, and reproduction steps. For RAK, include board count, scan range/step, configured packet frequency/BW/SF/sync word, per-board readiness, completed scans and CRC counts. For SDRs, include input rate and acquisition/discovery loss. Preview reports and screenshots locally before sharing. Exclude device identifiers, keys, packet bytes, message content, precise GPS routes, and raw RF captures.

Report suspected privacy or security defects through [SECURITY.md](SECURITY.md). Planned troubleshooting and incident processes are in [operations and support](docs/operations/support-playbook.md).
