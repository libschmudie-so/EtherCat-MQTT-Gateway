using System.Buffers.Binary;
using System.Text;
using EtherCAT.NET;
using EtherCAT.NET.Infrastructure;
using Newtonsoft.Json.Linq;

namespace EtherCatMqttGateway;

/// <summary>
/// Wraps an EtherCAT slave and exposes PDO variables as typed helpers.
/// Handles bit-level read/write and JSON conversions.
/// </summary>
public class SlaveDevice
{
    private readonly EcMaster _master;
    private readonly SlaveInfo _slaveInfo;
    private readonly ushort _slaveIndex; // ring order (1-based), used for MQTT CSA topics
    private readonly List<SlaveVariable> _variables = new();

    /// <summary>
    /// Creates a new slave device wrapper.
    /// </summary>
    /// <param name="master">EtherCAT master.</param>
    /// <param name="slaveInfo">Discovered slave info (from scan).</param>
    /// <param name="slaveIndex">Ring order starting at 1 (used as CSA/topic id).</param>
    public SlaveDevice(EcMaster master, SlaveInfo slaveInfo, ushort slaveIndex)
    {
        _master = master;
        _slaveInfo = slaveInfo;
        _slaveIndex = slaveIndex;

        foreach (var pdo in _slaveInfo.DynamicData.Pdos)
            _variables.AddRange(pdo.Variables);
    }

    /// <summary>Human-readable device name.</summary>
    public string GetName() => _slaveInfo.DynamicData.Name;

    /// <summary>
    /// Controller Station Address used for MQTT topics (ring index supplied by the controller).
    /// </summary>
    public ushort GetCsa() => _slaveIndex;

    /// <summary>
    /// CSA reported by the EtherCAT stack (may or may not match ring index).
    /// </summary>
    public ushort GetReportedCsa() => _slaveInfo.Csa;

    /// <summary>Returns the raw slave info.</summary>
    public SlaveInfo GetSlaveInfo() => _slaveInfo;

    /// <summary>
    /// Metadata for this slave (variables & attributes) as JSON.
    /// </summary>
    public JObject GetMetadata()
    {
        var pdos = new JArray();
        foreach (var variable in _variables)
        {
            var pdo = new JObject
            {
                ["name"] = variable.Name,
                ["bitLength"] = variable.BitLength,
                ["dataType"] = variable.DataType.ToString(),
                ["dataDirection"] = variable.DataDirection.ToString(),
                ["index"] = variable.Index.ToString("X4"),
                ["subIndex"] = variable.SubIndex.ToString("X2")
            };
            pdos.Add(pdo);
        }

        return new JObject
        {
            ["name"] = _slaveInfo.DynamicData.Name,
            ["description"] = _slaveInfo.DynamicData.Description,
            ["reportedCsa"] = _slaveInfo.Csa,
            ["ringCsa"] = _slaveIndex,
            ["pdos"] = pdos
        };
    }

    /// <summary>
    /// Whether the variable has a valid mapped data pointer.
    /// </summary>
    private static bool HasValidPtr(SlaveVariable v) => v.DataPtr != IntPtr.Zero;

    /// <summary>All mapped variables with valid pointers.</summary>
    public List<SlaveVariable> GetAllVariables() => _variables.Where(HasValidPtr).ToList();

    /// <summary>Output variables (Tx to slaves) with valid pointers.</summary>
    public List<SlaveVariable> GetOutputVariables() =>
        _variables.Where(v => v.DataDirection == DataDirection.Output && HasValidPtr(v)).ToList();

    /// <summary>Input variables (Rx from slaves) with valid pointers.</summary>
    public List<SlaveVariable> GetInputVariables() =>
        _variables.Where(v => v.DataDirection == DataDirection.Input && HasValidPtr(v)).ToList();

    /// <summary>
    /// Converts a bit-packed buffer to an unsigned integer (up to 64 bits, LSB first).
    /// </summary>
    private static ulong BufferToUInt(ReadOnlySpan<byte> buffer, byte bits)
    {
        if (bits is < 1 or > 64) throw new ArgumentOutOfRangeException(nameof(bits));
        ulong result = 0;

        for (int i = 0; i < bits; i++)
        {
            int byteIndex = i / 8;
            int bitIndex = i % 8;
            if ((buffer[byteIndex] & (1 << bitIndex)) != 0)
                result |= 1UL << i;
        }
        return result;
    }

    /// <summary>
    /// Converts a bit-packed buffer to a signed integer (up to 64 bits, LSB first).
    /// </summary>
    private static long BufferToInt(ReadOnlySpan<byte> buffer, byte bits)
    {
        ulong u = BufferToUInt(buffer, bits);
        long s = unchecked((long)u);
        if (bits < 64 && ((1L << (bits - 1)) & s) != 0)
            s |= -1L << bits; // sign-extend
        return s;
    }

    /// <summary>
    /// Packs an unsigned integer into a bit-packed buffer (LSB first).
    /// </summary>
    private static byte[] UIntToBuffer(ulong value, byte bits)
    {
        if (bits is < 1 or > 64) throw new ArgumentOutOfRangeException(nameof(bits));
        var buf = new byte[(bits + 7) / 8];
        for (int i = 0; i < bits; i++)
        {
            if ((value & (1UL << i)) != 0)
                buf[i / 8] |= (byte)(1 << (i % 8));
        }
        return buf;
    }

    /// <summary>
    /// Packs a signed integer into a bit-packed buffer (LSB first).
    /// </summary>
    private static byte[] IntToBuffer(long value, byte bits) => UIntToBuffer(unchecked((ulong)value), bits);

    /// <summary>
    /// Reads the variable into a tightly-sized bit-packed buffer (LSB-first).
    /// </summary>
    public byte[]? ReadVariable(SlaveVariable variable)
    {
        var buffer = new byte[(variable.BitLength + 7) / 8];

        unsafe
        {
            var basePtr = (byte*)variable.DataPtr.ToPointer();
            if (basePtr == null) return null;

            int byteOffset = variable.BitOffset / 8;
            int bitOffset = variable.BitOffset % 8;
            int bitLength = variable.BitLength;

            // Copy bit-by-bit from mapped process memory to our compact buffer.
            for (int i = 0; i < bitLength; i++)
            {
                int srcByte = (bitOffset + i) / 8;
                int srcBit = (bitOffset + i) % 8;

                if ((basePtr[byteOffset + srcByte] & (1 << srcBit)) != 0)
                    buffer[i / 8] |= (byte)(1 << (i % 8));
            }
        }
        return buffer;
    }

    /// <summary>
    /// Writes the given bit-packed buffer into the mapped variable (LSB-first).
    /// </summary>
    public void WriteVariable(SlaveVariable variable, ReadOnlySpan<byte> value)
    {
        unsafe
        {
            var basePtr = (byte*)variable.DataPtr.ToPointer();
            if (basePtr == null) throw new InvalidOperationException("Data pointer is null");

            int byteOffset = variable.BitOffset / 8;
            int bitOffset = variable.BitOffset % 8;
            int bitLength = variable.BitLength;

            for (int i = 0; i < bitLength; i++)
            {
                int dstByte = (bitOffset + i) / 8;
                int dstBit = (bitOffset + i) % 8;

                if ((value[i / 8] & (1 << (i % 8))) != 0)
                    basePtr[byteOffset + dstByte] |= (byte)(1 << dstBit);
                else
                    basePtr[byteOffset + dstByte] &= (byte)~(1 << dstBit);
            }
        }
    }

    /// <summary>
    /// Reads a variable and converts it to a JSON token (numbers/strings/byte arrays).
    /// </summary>
    public JToken ReadVariableAsJToken(SlaveVariable variable)
    {
        var buffer = ReadVariable(variable);
        if (buffer == null) return JValue.CreateNull();

        var span = buffer.AsSpan();

        switch (variable.DataType)
        {
            case EthercatDataType.Boolean:
                return new JValue((BufferToUInt(span, variable.BitLength) & 1UL) == 1UL);

            case EthercatDataType.BIT2:
            case EthercatDataType.BIT3:
            case EthercatDataType.BIT4:
            case EthercatDataType.BIT5:
            case EthercatDataType.BIT6:
            case EthercatDataType.BIT7:
            case EthercatDataType.BIT8:
            case EthercatDataType.BITARR8:
            case EthercatDataType.BITARR16:
            case EthercatDataType.BITARR32:
                return new JValue(BufferToUInt(span, variable.BitLength));

            case EthercatDataType.Unsigned8:
                return new JValue((byte)BufferToUInt(span, 8));
            case EthercatDataType.Unsigned16:
                return new JValue(BinaryPrimitives.ReadUInt16LittleEndian(span));
            case EthercatDataType.Unsigned24:
                return new JValue((uint)BufferToUInt(span, 24));
            case EthercatDataType.Unsigned32:
                return new JValue(BinaryPrimitives.ReadUInt32LittleEndian(span));
            case EthercatDataType.Unsigned40:
            case EthercatDataType.Unsigned48:
            case EthercatDataType.Unsigned56:
            case EthercatDataType.Unsigned64:
                return new JValue(BufferToUInt(span, variable.BitLength));

            case EthercatDataType.Integer8:
                return new JValue(unchecked((sbyte)BufferToInt(span, 8)));
            case EthercatDataType.Integer16:
                return new JValue(unchecked((short)BinaryPrimitives.ReadUInt16LittleEndian(span)));
            case EthercatDataType.Integer24:
                return new JValue(unchecked((int)BufferToInt(span, 24)));
            case EthercatDataType.Integer32:
                return new JValue(unchecked((int)BinaryPrimitives.ReadUInt32LittleEndian(span)));
            case EthercatDataType.Integer40:
            case EthercatDataType.Integer48:
            case EthercatDataType.Integer56:
            case EthercatDataType.Integer64:
                return new JValue(BufferToInt(span, variable.BitLength));

            case EthercatDataType.@float:
                // 32-bit IEEE-754 little-endian
                return new JValue(BitConverter.ToSingle(span));
            case EthercatDataType.@double:
                // 64-bit IEEE-754 little-endian
                return new JValue(BitConverter.ToDouble(span));

            case EthercatDataType.VisibleString:
                // Interpret as UTF-8/ASCII until first NUL.
                var nul = Array.IndexOf(buffer, (byte)0);
                var str = Encoding.UTF8.GetString(buffer, 0, nul >= 0 ? nul : buffer.Length);
                return new JValue(str);

            case EthercatDataType.OctetString:
                // Hex representation (safe for dashboards)
                return new JValue(Convert.ToHexString(buffer));

            // TODO: UnicodeString/GUID/Time types as needed
            default:
                return JValue.CreateNull();
        }
    }

    /// <summary>
    /// Converts a JSON token into the on-wire bit-packed representation and writes it.
    /// </summary>
    public void WriteVariableAsJToken(SlaveVariable variable, JToken value)
    {
        switch (variable.DataType)
        {
            case EthercatDataType.Boolean:
                WriteVariable(variable, UIntToBuffer(value.Value<bool>() ? 1UL : 0UL, variable.BitLength));
                return;

            case EthercatDataType.BIT2:
            case EthercatDataType.BIT3:
            case EthercatDataType.BIT4:
            case EthercatDataType.BIT5:
            case EthercatDataType.BIT6:
            case EthercatDataType.BIT7:
            case EthercatDataType.BIT8:
            case EthercatDataType.BITARR8:
            case EthercatDataType.BITARR16:
            case EthercatDataType.BITARR32:
                WriteVariable(variable, UIntToBuffer(value.Value<ulong>(), variable.BitLength));
                return;

            case EthercatDataType.Unsigned8:
            case EthercatDataType.Unsigned16:
            case EthercatDataType.Unsigned24:
            case EthercatDataType.Unsigned32:
            case EthercatDataType.Unsigned40:
            case EthercatDataType.Unsigned48:
            case EthercatDataType.Unsigned56:
            case EthercatDataType.Unsigned64:
                WriteVariable(variable, UIntToBuffer(value.Value<ulong>(), variable.BitLength));
                return;

            case EthercatDataType.Integer8:
            case EthercatDataType.Integer16:
            case EthercatDataType.Integer24:
            case EthercatDataType.Integer32:
            case EthercatDataType.Integer40:
            case EthercatDataType.Integer48:
            case EthercatDataType.Integer56:
            case EthercatDataType.Integer64:
                WriteVariable(variable, IntToBuffer(value.Value<long>(), variable.BitLength));
                return;

            case EthercatDataType.@float:
                {
                    var f = value.Value<float>();
                    Span<byte> tmp = stackalloc byte[4];
                    BinaryPrimitives.WriteSingleLittleEndian(tmp, f);
                    WriteVariable(variable, tmp);
                    return;
                }
            case EthercatDataType.@double:
                {
                    var d = value.Value<double>();
                    Span<byte> tmp = stackalloc byte[8];
                    BinaryPrimitives.WriteDoubleLittleEndian(tmp, d);
                    WriteVariable(variable, tmp);
                    return;
                }

            case EthercatDataType.VisibleString:
                {
                    var s = value.Value<string>() ?? string.Empty;
                    var bytes = Encoding.UTF8.GetBytes(s);
                    WriteVariable(variable, bytes);
                    return;
                }

            case EthercatDataType.OctetString:
                {
                    // Accept hex string (no spaces); otherwise treat as base64 if it looks like it.
                    var str = value.Value<string>() ?? "";
                    byte[] bytes;
                    if (str.Length % 2 == 0 && str.All(Uri.IsHexDigit))
                        bytes = Convert.FromHexString(str);
                    else
                        bytes = Convert.TryFromBase64String(str, new Span<byte>(new byte[str.Length]), out _)
                            ? Convert.FromBase64String(str)
                            : Encoding.UTF8.GetBytes(str);
                    WriteVariable(variable, bytes);
                    return;
                }

            default:
                // Silently ignore unsupported/opaque types for now.
                return;
        }
    }

    /// <summary>
    /// Legacy strongly-typed getters: kept for compatibility; prefer JSON APIs.
    /// </summary>
    public object? GetVariableValue(SlaveVariable variable)
    {
        if (variable.DataType == 0) return null;
        var buf = ReadVariable(variable);
        if (buf == null) return null;

        var span = buf.AsSpan();

        switch (variable.DataType)
        {
            case EthercatDataType.Boolean:
                return (BufferToUInt(span, variable.BitLength) & 1UL) == 1UL;

            case EthercatDataType.BIT2:
            case EthercatDataType.BIT3:
            case EthercatDataType.BIT4:
            case EthercatDataType.BIT5:
            case EthercatDataType.BIT6:
            case EthercatDataType.BIT7:
            case EthercatDataType.BIT8:
                // Return packed bits as ulong
                return BufferToUInt(span, variable.BitLength);

            case EthercatDataType.BITARR8:
            case EthercatDataType.Unsigned8:
                return (byte)BufferToUInt(span, 8);

            case EthercatDataType.BITARR16:
            case EthercatDataType.Unsigned16:
                return BinaryPrimitives.ReadUInt16LittleEndian(span);

            case EthercatDataType.BITARR32:
            case EthercatDataType.Unsigned32:
                return BinaryPrimitives.ReadUInt32LittleEndian(span);

            case EthercatDataType.Unsigned64:
                return BinaryPrimitives.ReadUInt64LittleEndian(span);

            case EthercatDataType.@float:
                return BitConverter.ToSingle(span);

            case EthercatDataType.@double:
                return BitConverter.ToDouble(span);

            case EthercatDataType.Integer8:
                return unchecked((sbyte)BufferToInt(span, 8));

            case EthercatDataType.Integer16:
                return unchecked((short)BinaryPrimitives.ReadUInt16LittleEndian(span));

            case EthercatDataType.Integer32:
                return unchecked((int)BinaryPrimitives.ReadUInt32LittleEndian(span));

            case EthercatDataType.Integer64:
                return unchecked((long)BinaryPrimitives.ReadUInt64LittleEndian(span));

            default:
                throw new ArgumentOutOfRangeException(nameof(variable.DataType), variable.DataType, "Unsupported data type");
        }
    }

    /// <summary>
    /// Legacy strongly-typed setter: prefer WriteVariableAsJToken.
    /// </summary>
    public void SetVariableValue(SlaveVariable variable, object value)
    {
        if (variable.DataType == 0) return;

        switch (variable.DataType)
        {
            case EthercatDataType.Boolean:
                WriteVariable(variable, UIntToBuffer(((bool)value) ? 1UL : 0UL, 1));
                break;

            case EthercatDataType.BIT2:
            case EthercatDataType.BIT3:
            case EthercatDataType.BIT4:
            case EthercatDataType.BIT5:
            case EthercatDataType.BIT6:
            case EthercatDataType.BIT7:
            case EthercatDataType.BIT8:
                WriteVariable(variable, UIntToBuffer(Convert.ToUInt64(value), variable.BitLength));
                break;

            case EthercatDataType.BITARR8:
            case EthercatDataType.Unsigned8:
                WriteVariable(variable, new[] { Convert.ToByte(value) });
                break;

            case EthercatDataType.BITARR16:
            case EthercatDataType.Unsigned16:
                {
                    Span<byte> tmp = stackalloc byte[2];
                    BinaryPrimitives.WriteUInt16LittleEndian(tmp, Convert.ToUInt16(value));
                    WriteVariable(variable, tmp);
                    break;
                }

            case EthercatDataType.BITARR32:
            case EthercatDataType.Unsigned32:
                {
                    Span<byte> tmp = stackalloc byte[4];
                    BinaryPrimitives.WriteUInt32LittleEndian(tmp, Convert.ToUInt32(value));
                    WriteVariable(variable, tmp);
                    break;
                }

            case EthercatDataType.Unsigned64:
                {
                    Span<byte> tmp = stackalloc byte[8];
                    BinaryPrimitives.WriteUInt64LittleEndian(tmp, Convert.ToUInt64(value));
                    WriteVariable(variable, tmp);
                    break;
                }

            case EthercatDataType.@float:
                {
                    Span<byte> tmp = stackalloc byte[4];
                    BinaryPrimitives.WriteSingleLittleEndian(tmp, Convert.ToSingle(value));
                    WriteVariable(variable, tmp);
                    break;
                }

            case EthercatDataType.@double:
                {
                    Span<byte> tmp = stackalloc byte[8];
                    BinaryPrimitives.WriteDoubleLittleEndian(tmp, Convert.ToDouble(value));
                    WriteVariable(variable, tmp);
                    break;
                }

            case EthercatDataType.Integer8:
                WriteVariable(variable, new[] { unchecked((byte)Convert.ToSByte(value)) });
                break;

            case EthercatDataType.Integer16:
                {
                    Span<byte> tmp = stackalloc byte[2];
                    BinaryPrimitives.WriteInt16LittleEndian(tmp, Convert.ToInt16(value));
                    WriteVariable(variable, tmp);
                    break;
                }

            case EthercatDataType.Integer32:
                {
                    Span<byte> tmp = stackalloc byte[4];
                    BinaryPrimitives.WriteInt32LittleEndian(tmp, Convert.ToInt32(value));
                    WriteVariable(variable, tmp);
                    break;
                }

            case EthercatDataType.Integer64:
                {
                    Span<byte> tmp = stackalloc byte[8];
                    BinaryPrimitives.WriteInt64LittleEndian(tmp, Convert.ToInt64(value));
                    WriteVariable(variable, tmp);
                    break;
                }

            default:
                throw new ArgumentOutOfRangeException(nameof(variable.DataType), variable.DataType, "Unsupported data type");
        }
    }
}
