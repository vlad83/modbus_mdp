#include "Master.h"
#include "crc.h"

#include <iomanip>
#include <iostream>
#include <thread>

namespace Modbus {
namespace RTU {


namespace {

using ByteSeq = Master::ByteSeq;
using DataSeq  = Master::DataSeq;

constexpr const uint8_t FCODE_RD_HOLDING_REGISTERS = 3;
constexpr const uint8_t FCODE_WR_REGISTER = 6;
constexpr const uint8_t FCODE_WR_REGISTERS = 16;
constexpr const uint8_t FCODE_USER1_OFFSET = 65;
constexpr const uint8_t FCODE_RD_BYTES = FCODE_USER1_OFFSET + 0;
constexpr const uint8_t FCODE_WR_BYTES = FCODE_USER1_OFFSET + 1;

const int gDebug =
    ::getenv("DEBUG")
    ? ::atoi(getenv("DEBUG"))
    : 0;

uint8_t lowByte(uint16_t word)
{
    return word & 0xFF;
}

uint8_t highByte(uint16_t word)
{
    return word >> 8;
}

void dump(std::ostream &os, const uint8_t *begin, const uint8_t *const end)
{
    const auto flags = os.flags();

    while(begin != end)
    {
        os
            << '['
            << std::hex << std::setw(2) << std::setfill('0') << int(*begin)
            << ']';
        ++begin;
    }
    os.flags(flags);
}

enum class DataSource
{
    Master, Slave
};

void debug(
    DataSource dataSource,
    const char *tag,
    const uint8_t *begin, const uint8_t *const end,
    const uint8_t *const curr)
{
    if(!gDebug) return;

    if(DataSource::Master == dataSource) std::cout << ">";
    else if(DataSource::Slave == dataSource) std::cout << "<";

    if(curr != end)
    {
        if(begin == curr) std::cout << "timeout\n";
        else if(
            std::distance(begin, curr) == 4 /* addr + fcode + crc */
            && 0x80 < *std::next(begin))
        {
            std::cout
                << "exception fcode " << int(*std::next(begin)) << "\n";
        }
        else if(
            std::distance(begin, curr) == 5 /* addr + fcode + ecode + crc */
            && 0x80 < *std::next(begin))
        {
            std::cout
                << "exception fcode " << int(*std::next(begin))
                << " ecode " << int(*std::next(begin, 2)) << "\n";
        }
        else std::cout << "unsupported (partial reply?)\n";
    }
    dump(std::cout, begin, end);
    if(1 < gDebug) std::cout << " " << tag;
    std::cout << std::endl;
}

ByteSeq &append(ByteSeq &seq, uint16_t word)
{
    seq.push_back(highByte(word));
    seq.push_back(lowByte(word));
    return seq;
}

ByteSeq toByteSeq(const DataSeq &dataSeq)
{
    ByteSeq byteSeq;

    for(auto data : dataSeq) append(byteSeq, data);
    return byteSeq;
}

DataSeq toDataSeq(const ByteSeq &byteSeq)
{
    ENSURE(0u == (byteSeq.size() & 1), RuntimeError);
    DataSeq dataSeq;

    for(auto i = std::begin(byteSeq); i != std::end(byteSeq);)
    {
        const uint16_t highByte = *i++;
        const uint16_t lowByte = *i++;
        dataSeq.push_back((highByte << 8) | lowByte);
    }
    return dataSeq;
}

void append(ByteSeq &dst, const ByteSeq &src)
{
    dst.insert(std::end(dst), std::begin(src), std::end(src));
}

void append(ByteSeq &dst, const DataSeq &src)
{
    append(dst, toByteSeq(src));
}

ByteSeq &appendCRC(ByteSeq &seq)
{
    auto begin = seq.data();
    auto end = begin + seq.size();
    auto crc = calcCRC(begin, end);

    seq.push_back(highByte(crc.value));
    seq.push_back(lowByte(crc.value));
    return seq;
}

void validateCRC(const ByteSeq &seq)
{
    if(seq.empty()) return;

    ENSURE(2u < seq.size(), RuntimeError);

    auto begin = seq.data();
    auto end = begin + seq.size();
    auto crc = calcCRC(begin, std::next(begin, seq.size() - 2));

    ENSURE(lowByte(crc.value) == *seq.rbegin(), RuntimeError);
    ENSURE(highByte(crc.value) == *++seq.rbegin(), RuntimeError);
}

} /* namespace */

void Master::wrRegister(
    Addr slaveAddr,
    uint16_t memAddr,
    uint16_t data,
    mSecs timeout)
{
    SerialPort dev{devName_, baudRate_, parity_, dataBits_, stopBits_};

    ByteSeq req
    {
        slaveAddr.value,
        FCODE_WR_REGISTER,
        highByte(memAddr),
        lowByte(memAddr),
        highByte(data),
        lowByte(data)
    };

    const auto reqSize = req.size();

    appendCRC(req);

    // request
    {
        const auto reqBegin = req.data();
        const auto reqEnd = reqBegin + req.size();
        const auto r = dev.write(reqBegin, reqEnd, mSecs{0});

        debug(DataSource::Master, __PRETTY_FUNCTION__, reqBegin, reqEnd, r);
        ENSURE(reqEnd == r, RuntimeError);
    }

    ByteSeq rep(reqSize + sizeof(CRC), 0);

    // reply
    {
        const auto repBegin = rep.data();
        const auto repEnd = repBegin + rep.size();
        const auto r = dev.read(repBegin, repEnd, timeout);

        debug(DataSource::Slave, __PRETTY_FUNCTION__, repBegin, repEnd, r);
        ENSURE(repEnd == r, RuntimeError);
    }

    validateCRC(rep);
    ENSURE(
        std::equal(
            std::begin(rep), std::next(std::begin(rep), rep.size() - sizeof(CRC)),
            std::begin(req)),
        RuntimeError);
}

void Master::wrRegisters(
    Addr slaveAddr,
    uint16_t memAddr,
    const DataSeq &dataSeq,
    mSecs timeout)
{
    if(dataSeq.empty()) return;

    ENSURE(0x7C > dataSeq.size(), RuntimeError);

    SerialPort dev{devName_, baudRate_, parity_, dataBits_, stopBits_};

    ByteSeq req
    {
        slaveAddr.value,
        FCODE_WR_REGISTERS,
        highByte(memAddr), lowByte(memAddr), /* starting address */
        highByte(dataSeq.size()), lowByte(dataSeq.size()), /* quantity of registers */
        uint8_t(dataSeq.size() << 1) /* byte_count */
    };

    append(req, dataSeq);
    appendCRC(req);

    // request
    {
        const auto reqBegin = req.data();
        const auto reqEnd = reqBegin + req.size();
        const auto r = dev.write(reqBegin, reqEnd, mSecs{0});

        debug(DataSource::Master, __PRETTY_FUNCTION__, reqBegin, reqEnd, r);
        ENSURE(reqEnd == r, RuntimeError);
    }

    ByteSeq rep(
        1 /* addr */
        + 1 /* fcode */
        + 2 /* starting address */
        + 2 /* quantity of registers */
        + sizeof(CRC), 0);

    // reply
    {
        const auto repBegin = rep.data();
        const auto repEnd = repBegin + rep.size();
        const auto r = dev.read(repBegin, repEnd, timeout);

        debug(DataSource::Slave, __PRETTY_FUNCTION__, repBegin, repEnd, r);
        ENSURE(repEnd == r, RuntimeError);
    }

    validateCRC(rep);
    ENSURE(
        std::equal(
            std::begin(rep), std::next(std::begin(rep), rep.size() - sizeof(CRC)),
            std::begin(req)),
        RuntimeError);
}

DataSeq Master::rdRegisters(
    Addr slaveAddr,
    uint16_t memAddr,
    uint8_t count,
    mSecs timeout)
{
    ENSURE(0 < count, RuntimeError);
    ENSURE(0x7E > count, RuntimeError);

    SerialPort dev{devName_, baudRate_, parity_, dataBits_, stopBits_};

    ByteSeq req
    {
        slaveAddr.value,
        FCODE_RD_HOLDING_REGISTERS,
        highByte(memAddr),
        lowByte(memAddr),
        highByte(count),
        lowByte(count)
    };

    appendCRC(req);

    // request
    {
        const auto reqBegin = req.data();
        const auto reqEnd = reqBegin + req.size();
        const auto r = dev.write(reqBegin, reqEnd, mSecs{0});

        debug(DataSource::Master, __PRETTY_FUNCTION__, reqBegin, reqEnd, r);
        ENSURE(reqEnd == r, RuntimeError);
    }

    dev.drain();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    constexpr const auto repHeaderSize = 1 /* slave */ + 1 /* fcode */ + 1 /* byte count */;
    const auto repSize = repHeaderSize + (count << 1)  /* data[] */ + sizeof(CRC);
    ByteSeq rep(repSize, 0);

    // reply
    {
        const auto repBegin = rep.data();
        const auto repEnd = repBegin + rep.size();
        const auto r = dev.read(repBegin, repEnd, timeout);

        debug(DataSource::Slave, __PRETTY_FUNCTION__, repBegin, repEnd, r);
        ENSURE(repEnd == r, RuntimeError);
    }

    validateCRC(rep);
    ENSURE(rep[0] == slaveAddr.value, RuntimeError);
    ENSURE(rep[1] == FCODE_RD_HOLDING_REGISTERS, RuntimeError);

    auto dataSeq =
        toDataSeq(
            ByteSeq
            {
                std::next(std::begin(rep), repHeaderSize),
                std::next(std::begin(rep), rep.size() - sizeof(CRC))
            });
    return dataSeq;
}

void Master::wrBytes(
    Addr slaveAddr,
    uint16_t memAddr,
    const ByteSeq &byteSeq,
    mSecs timeout)
{
    if(byteSeq.empty()) return;

    ENSURE(250u > byteSeq.size(), RuntimeError);

    SerialPort dev{devName_, baudRate_, parity_, dataBits_, stopBits_};

    ByteSeq req
    {
        slaveAddr.value,
        FCODE_WR_BYTES,
        highByte(memAddr),
        lowByte(memAddr),
        uint8_t(byteSeq.size())
    };

    const auto reqSize = req.size();

    append(req, byteSeq);
    appendCRC(req);

    // request
    {
        const auto reqBegin = req.data();
        const auto reqEnd = reqBegin + req.size();
        const auto r = dev.write(reqBegin, reqEnd, mSecs{0});

        debug(DataSource::Slave, __PRETTY_FUNCTION__, reqBegin, reqEnd, r);
        ENSURE(reqEnd == r, RuntimeError);
    }

    ByteSeq rep(reqSize + sizeof(CRC), 0);

    // reply
    {
        const auto repBegin = rep.data();
        const auto repEnd = repBegin + rep.size();
        const auto r = dev.read(repBegin, repEnd, timeout);

        debug(DataSource::Slave, __PRETTY_FUNCTION__, repBegin, repEnd, r);
        ENSURE(repEnd == r, RuntimeError);
    }

    validateCRC(rep);
    ENSURE(
        std::equal(
            std::begin(rep), std::next(std::begin(rep), rep.size() - sizeof(CRC)),
            std::begin(req)),
        RuntimeError);
}

} /* RTU */
} /* Modbus */
