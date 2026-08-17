"""Data models for the ECGW GUI."""

from typing import Dict, List, Any, Optional
from dataclasses import dataclass, field


@dataclass
class Signal:
    """Represents a single EtherCAT signal."""
    index: str
    subindex: str
    name: str
    description: str
    data_type: str
    data_direction: str  # "Input" or "Output"
    bit_length: int
    value: str = ""
    
    def get_full_key(self) -> str:
        """Get the full key for this signal."""
        return f"{self.index}/{self.subindex}"


@dataclass
class PdoOption:
    """One ESI-declared RxPdo/TxPdo option -- not necessarily the one
    currently assigned. See --pdo-config in the gateway's README."""
    index: str  # hex "0x..." form
    name: str = ""

    def label(self) -> str:
        """Human-readable label for a picker widget."""
        return f"{self.name} ({self.index})" if self.name else self.index

    def selector(self) -> str:
        """Value to use as a --pdo-config rxPdo/txPdo selector -- prefer
        the declared name (more readable in the generated JSON), fall back
        to the index if ESI didn't declare one."""
        return self.name if self.name else self.index


@dataclass
class Device:
    """Represents an EtherCAT device/slave."""
    device_id: int
    name: str
    description: str
    state: str
    error: bool = False
    alarm_code: str = ""
    alarm: str = ""
    vendor_id: str = ""
    product_code: str = ""
    revision_no: str = ""
    signals: Dict[str, Signal] = field(default_factory=dict)
    available_rx_pdos: List[PdoOption] = field(default_factory=list)
    available_tx_pdos: List[PdoOption] = field(default_factory=list)
    metadata: Dict[str, Any] = field(default_factory=dict)
    
    def add_signal(self, signal: Signal):
        """Add a signal to this device."""
        self.signals[signal.get_full_key()] = signal
    
    def get_signal(self, index: str, subindex: str) -> Optional[Signal]:
        """Get a signal by index and subindex."""
        key = f"{index}/{subindex}"
        return self.signals.get(key)


@dataclass
class Bridge:
    """Represents the EtherCAT bridge."""
    status: bool = False
    info: Dict[str, Any] = field(default_factory=dict)
    devices: Dict[int, Device] = field(default_factory=dict)
    
    def add_device(self, device: Device):
        """Add a device to the bridge."""
        self.devices[device.device_id] = device
    
    def get_device(self, device_id: int) -> Optional[Device]:
        """Get a device by ID."""
        return self.devices.get(device_id)
    
    def update_status(self, status: bool):
        """Update the bridge status."""
        self.status = status
    
    def update_info(self, info: Dict[str, Any]):
        """Update the bridge info."""
        self.info = info
        
        # Get current slave IDs from info
        current_slave_ids = set()
        if "slaves" in info:
            current_slave_ids = set(int(slave_id_str) for slave_id_str in info["slaves"].keys())
            
            # Add or update devices
            for slave_id_str, slave_info in info["slaves"].items():
                slave_id = int(slave_id_str)
                if slave_id not in self.devices:
                    device = Device(
                        device_id=slave_id,
                        name=slave_info.get("name", f"Slave {slave_id}"),
                        description=slave_info.get("description", ""),
                        state=slave_info.get("state", "Unknown"),
                        error=slave_info.get("error", False),
                        alarm_code=slave_info.get("alarmCode", ""),
                        alarm=slave_info.get("alarm", ""),
                        metadata=slave_info
                    )
                    self.add_device(device)
                else:
                    # Update existing device
                    device = self.devices[slave_id]
                    device.name = slave_info.get("name", device.name)
                    device.description = slave_info.get("description", device.description)
                    device.state = slave_info.get("state", device.state)
                    device.error = slave_info.get("error", device.error)
                    device.alarm_code = slave_info.get("alarmCode", device.alarm_code)
                    device.alarm = slave_info.get("alarm", device.alarm)
                    device.metadata = slave_info
        
        # Remove devices that are no longer in the slaves list
        removed_ids = set(self.devices.keys()) - current_slave_ids
        for device_id in removed_ids:
            del self.devices[device_id]
    
    def update_device_metadata(self, device_id: int, metadata: Dict[str, Any]):
        """Update device metadata and signals."""
        if device_id in self.devices:
            device = self.devices[device_id]
            device.metadata = metadata
            device.name = metadata.get("name", device.name)
            device.description = metadata.get("description", device.description)
            device.state = metadata.get("state", device.state)
            device.error = metadata.get("error", device.error)
            device.alarm_code = metadata.get("alarmCode", device.alarm_code)
            device.alarm = metadata.get("alarm", device.alarm)
            device.vendor_id = metadata.get("vendorId", device.vendor_id)
            device.product_code = metadata.get("productCode", device.product_code)
            device.revision_no = metadata.get("revisionNo", device.revision_no)
            device.available_rx_pdos = [
                PdoOption(index=p.get("index", ""), name=p.get("name", ""))
                for p in metadata.get("availableRxPdos", [])
            ]
            device.available_tx_pdos = [
                PdoOption(index=p.get("index", ""), name=p.get("name", ""))
                for p in metadata.get("availableTxPdos", [])
            ]

            # Clear old signals and parse new PDOs (Process Data Objects)
            device.signals.clear()
            if "pdos" in metadata:
                for pdo in metadata["pdos"]:
                    signal = Signal(
                        index=pdo.get("index", ""),
                        subindex=pdo.get("subIndex", ""),
                        name=pdo.get("name", ""),
                        description=pdo.get("description", ""),
                        data_type=pdo.get("dataType", ""),
                        data_direction=pdo.get("dataDirection", ""),
                        bit_length=pdo.get("bitLength", 0)
                    )
                    device.add_signal(signal)
