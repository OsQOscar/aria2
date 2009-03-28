/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2006 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#include "BitfieldMan.h"

#include <cassert>
#include <cstring>

#include "Randomizer.h"
#include "Util.h"
#include "array_fun.h"
#include "bitfield.h"

namespace aria2 {

BitfieldMan::BitfieldMan(size_t blockLength, uint64_t totalLength)
  :blockLength(blockLength),
   totalLength(totalLength),
   bitfield(0),
   useBitfield(0),
   filterBitfield(0),
   bitfieldLength(0),
   blocks(0),
   filterEnabled(false),
   cachedNumMissingBlock(0),
   cachedNumFilteredBlock(0),
   cachedCompletedLength(0),
   cachedFilteredComletedLength(0),
   cachedFilteredTotalLength(0)
{
  if(blockLength > 0 && totalLength > 0) {
    blocks = totalLength/blockLength+(totalLength%blockLength ? 1 : 0);
    bitfieldLength = blocks/8+(blocks%8 ? 1 : 0);
    bitfield = new unsigned char[bitfieldLength];
    useBitfield = new unsigned char[bitfieldLength];
    memset(bitfield, 0, bitfieldLength);
    memset(useBitfield, 0, bitfieldLength);
    updateCache();
  }
}

BitfieldMan::BitfieldMan(const BitfieldMan& bitfieldMan)
  :blockLength(0),
   totalLength(0),
   bitfield(0),
   useBitfield(0),
   filterBitfield(0),
   bitfieldLength(0),
   blocks(0),
   filterEnabled(false),
   cachedNumMissingBlock(0),
   cachedNumFilteredBlock(0),
   cachedCompletedLength(0),
   cachedFilteredComletedLength(0),
   cachedFilteredTotalLength(0)
{
  blockLength = bitfieldMan.blockLength;
  totalLength = bitfieldMan.totalLength;
  blocks = bitfieldMan.blocks;
  bitfieldLength = bitfieldMan.bitfieldLength;
  bitfield = new unsigned char[bitfieldLength];
  useBitfield = new unsigned char[bitfieldLength];
  memcpy(bitfield, bitfieldMan.bitfield, bitfieldLength);
  memcpy(useBitfield, bitfieldMan.useBitfield, bitfieldLength);
  filterEnabled = bitfieldMan.filterEnabled;
  if(filterBitfield) {
    filterBitfield = new unsigned char[bitfieldLength];
    memcpy(filterBitfield, bitfieldMan.filterBitfield, bitfieldLength);
  } else {
    filterBitfield = 0;
  }
  this->randomizer = bitfieldMan.randomizer;
  updateCache();
}

BitfieldMan& BitfieldMan::operator=(const BitfieldMan& bitfieldMan)
{
  if(this != &bitfieldMan) {
    blockLength = bitfieldMan.blockLength;
    totalLength = bitfieldMan.totalLength;
    blocks = bitfieldMan.blocks;
    bitfieldLength = bitfieldMan.bitfieldLength;
    filterEnabled = bitfieldMan.filterEnabled;

    delete [] bitfield;
    bitfield = new unsigned char[bitfieldLength];
    memcpy(bitfield, bitfieldMan.bitfield, bitfieldLength);

    delete [] useBitfield;
    useBitfield = new unsigned char[bitfieldLength];
    memcpy(useBitfield, bitfieldMan.useBitfield, bitfieldLength);

    delete [] filterBitfield;
    if(filterEnabled) {
      filterBitfield = new unsigned char[bitfieldLength];
      memcpy(filterBitfield, bitfieldMan.filterBitfield, bitfieldLength);
    } else {
      filterBitfield = 0;
    }

    updateCache();
  }
  return *this;
}

BitfieldMan::~BitfieldMan() {
  delete [] bitfield;
  delete [] useBitfield;
  delete [] filterBitfield;
}

size_t BitfieldMan::getBlockLength() const
{
  return blockLength;
}

size_t BitfieldMan::getLastBlockLength() const
{
  return totalLength-blockLength*(blocks-1);
}

size_t BitfieldMan::getBlockLength(size_t index) const
{
  if(index == blocks-1) {
    return getLastBlockLength();
  } else if(index < blocks-1) {
    return getBlockLength();
  } else {
    return 0;
  }
}

size_t
BitfieldMan::getNthBitIndex(const unsigned char bitfield, size_t nth) const
{
  size_t index = 0;
  for(int bs = 7; bs >= 0; --bs) {
    unsigned char mask = 1 << bs;
    if(bitfield & mask) {
      nth--;
      if(nth == 0) {
	index = 7-bs;
	break;
      }
    }
  }
  return index;
}

template<typename Array>
bool BitfieldMan::getMissingIndexRandomly(size_t& index,
					  const Array& bitfield,
					  size_t bitfieldLength) const
{
  size_t byte = randomizer->getRandomNumber(bitfieldLength);
  for(size_t i = 0; i < bitfieldLength; ++i) {
    unsigned char mask;
    if(byte == bitfieldLength-1) {
      mask = bitfield::lastByteMask(blocks);
    } else {
      mask = 0xff;
    }
    unsigned char bits = bitfield[byte];

    if(bits&mask) {
      index = byte*8+getNthBitIndex(bits, 1);
      return true;
    }
    ++byte;
    if(byte == bitfieldLength) {
      byte = 0;
    }
  }
  return false;
}

bool BitfieldMan::hasMissingPiece(const unsigned char* peerBitfield, size_t length) const {
  if(bitfieldLength != length) {
    return false;
  }
  bool retval = false;
  for(size_t i = 0; i < bitfieldLength; ++i) {
    unsigned char temp = peerBitfield[i] & ~bitfield[i];
    if(filterEnabled) {
      temp &= filterBitfield[i];
    }
    if(temp&0xff) {
      retval = true;
      break;
    }
  }
  return retval;
}

bool BitfieldMan::getMissingIndex(size_t& index, const unsigned char* peerBitfield, size_t length) const {
  if(bitfieldLength != length) {
    return false;
  }
  array_fun<unsigned char> bf = array_and(array_negate(bitfield), peerBitfield);
  if(filterEnabled) {
    bf = array_and(bf, filterBitfield);
  }
  return getMissingIndexRandomly(index, bf, bitfieldLength);
}

bool BitfieldMan::getMissingUnusedIndex(size_t& index, const unsigned char* peerBitfield, size_t length) const {
  if(bitfieldLength != length) {
    return false;
  }
  array_fun<unsigned char> bf = array_and(array_and(array_negate(bitfield),
						    array_negate(useBitfield)),
					  peerBitfield);
  if(filterEnabled) {
    bf = array_and(bf, filterBitfield);
  }
  return getMissingIndexRandomly(index, bf, bitfieldLength);
}

template<typename Array>
bool BitfieldMan::getFirstMissingIndex(size_t& index, const Array& bitfield, size_t bitfieldLength) const
{
  for(size_t i = 0; i < bitfieldLength; ++i) {
    unsigned char bits = bitfield[i];
    unsigned char mask = 128;
    size_t tindex = i*8;
    for(size_t bi = 0; bi < 8 && tindex < blocks; ++bi, mask >>= 1, ++tindex) {
      if(bits & mask) {
	index = tindex;
	return true;
      }
    }
  }
  return false;
}

bool BitfieldMan::getFirstMissingUnusedIndex(size_t& index) const
{
  array_fun<unsigned char> bf = array_and(array_negate(bitfield),
					  array_negate(useBitfield));
  if(filterEnabled) {
    bf = array_and(bf, filterBitfield);
  }
  return getFirstMissingIndex(index, bf, bitfieldLength);
}

bool BitfieldMan::getFirstMissingIndex(size_t& index) const
{
  array_fun<unsigned char> bf = array_negate(bitfield);
  if(filterEnabled) {
    bf = array_and(bf, filterBitfield);
  }
  return getFirstMissingIndex(index, bf, bitfieldLength);
}

bool BitfieldMan::getMissingIndex(size_t& index) const {
  array_fun<unsigned char> bf = array_negate(bitfield);
  if(filterEnabled) {
    bf = array_and(bf, filterBitfield);
  }
  return getMissingIndexRandomly(index, bf, bitfieldLength);
}

bool BitfieldMan::getMissingUnusedIndex(size_t& index) const
{
  array_fun<unsigned char> bf = array_and(array_negate(bitfield),
					  array_negate(useBitfield));
  if(filterEnabled) {
    bf = array_and(bf, filterBitfield);
  }
  return getMissingIndexRandomly(index, bf, bitfieldLength);
}

size_t BitfieldMan::getStartIndex(size_t index) const {
  while(index < blocks && (isUseBitSet(index) || isBitSet(index))) {
    index++;
  }
  if(blocks <= index) {
    return blocks;
  } else {
    return index;
  }
}

size_t BitfieldMan::getEndIndex(size_t index) const {
  while(index < blocks && (!isUseBitSet(index) && !isBitSet(index))) {
    index++;
  }
  return index;
}

bool BitfieldMan::getSparseMissingUnusedIndex(size_t& index) const {
  Range maxRange;
  Range currentRange;
  {
    size_t nextIndex = 0;
    while(nextIndex < blocks) {
      currentRange.startIndex = getStartIndex(nextIndex);
      if(currentRange.startIndex == blocks) {
	break;
      }
      currentRange.endIndex = getEndIndex(currentRange.startIndex);
      if(maxRange < currentRange) {
	maxRange = currentRange;
      }
      nextIndex = currentRange.endIndex;
    }
  }
  if(maxRange.getSize()) {
    if(maxRange.startIndex == 0) {
      index = 0;
    } else if(isUseBitSet(maxRange.startIndex-1)) {
      index = maxRange.getMidIndex();
    } else {
      index = maxRange.startIndex;
    }
    return true;
  } else {
    return false;
  }
}

template<typename Array>
static bool copyBitfield(unsigned char* dst, const Array& src, size_t blocks)
{
  unsigned char bits = 0;
  size_t len = (blocks+7)/8;
  for(size_t i = 0; i < len-1; ++i) {
    dst[i] = src[i];
    bits |= dst[i];
  }
  dst[len-1] = src[len-1]&bitfield::lastByteMask(blocks);
  bits |= dst[len-1];
  return bits != 0;
}

bool BitfieldMan::getAllMissingIndexes(unsigned char* misbitfield, size_t len)
  const
{
  assert(len == bitfieldLength);
  array_fun<unsigned char> bf = array_negate(bitfield);
  if(filterEnabled) {
    bf = array_and(bf, filterBitfield);
  }
  return copyBitfield(misbitfield, bf, blocks);
}

bool BitfieldMan::getAllMissingIndexes(unsigned char* misbitfield, size_t len,
				       const unsigned char* peerBitfield,
				       size_t peerBitfieldLength) const
{
  assert(len == bitfieldLength);
  if(bitfieldLength != peerBitfieldLength) {
    return false;
  }
  array_fun<unsigned char> bf = array_and(array_negate(bitfield),
					  peerBitfield);
  if(filterEnabled) {
    bf = array_and(bf, filterBitfield);
  }
  return copyBitfield(misbitfield, bf, blocks);
}

bool BitfieldMan::getAllMissingUnusedIndexes(unsigned char* misbitfield,
					     size_t len,
					     const unsigned char* peerBitfield,
					     size_t peerBitfieldLength) const
{
  assert(len == bitfieldLength);
  if(bitfieldLength != peerBitfieldLength) {
    return false;
  }
  array_fun<unsigned char> bf = array_and(array_and(array_negate(bitfield),
						    array_negate(useBitfield)),
					  peerBitfield);
  if(filterEnabled) {
    bf = array_and(bf, filterBitfield);
  }
  return copyBitfield(misbitfield, bf, blocks);
}

size_t BitfieldMan::countMissingBlock() const {
  return cachedNumMissingBlock;
}

size_t BitfieldMan::countMissingBlockNow() const {
  if(filterEnabled) {
    array_ptr<unsigned char> temp(new unsigned char[bitfieldLength]);
    for(size_t i = 0; i < bitfieldLength; ++i) {
      temp[i] = bitfield[i]&filterBitfield[i];
    }
    size_t count =  bitfield::countSetBit(filterBitfield, blocks)-
      bitfield::countSetBit(temp, blocks);
    return count;
  } else {
    return blocks-bitfield::countSetBit(bitfield, blocks);
  }
}

size_t BitfieldMan::countFilteredBlock() const {
  return cachedNumFilteredBlock;
}

size_t BitfieldMan::countBlock() const {
  return blocks;
}

size_t BitfieldMan::countFilteredBlockNow() const {
  if(filterEnabled) {
    return bitfield::countSetBit(filterBitfield, blocks);
  } else {
    return 0;
  }
}

size_t BitfieldMan::getMaxIndex() const
{
  return blocks-1;
}

bool BitfieldMan::setBitInternal(unsigned char* bitfield, size_t index, bool on) {
  if(blocks <= index) { return false; }
  unsigned char mask = 128 >> index%8;
  if(on) {
    bitfield[index/8] |= mask;
  } else {
    bitfield[index/8] &= ~mask;
  }
  return true;
}

bool BitfieldMan::setUseBit(size_t index) {
  return setBitInternal(useBitfield, index, true);
}

bool BitfieldMan::unsetUseBit(size_t index) {
  return setBitInternal(useBitfield, index, false);
}

bool BitfieldMan::setBit(size_t index) {
  bool b = setBitInternal(bitfield, index, true);
  updateCache();
  return b;
}

bool BitfieldMan::unsetBit(size_t index) {
  bool b = setBitInternal(bitfield, index, false);
  updateCache();
  return b;
}

bool BitfieldMan::isFilteredAllBitSet() const {
  if(filterEnabled) {
    for(size_t i = 0; i < bitfieldLength; ++i) {
      if((bitfield[i]&filterBitfield[i]) != filterBitfield[i]) {
	return false;
      }
    }
    return true;
  } else {
    return isAllBitSet();
  }
}

bool BitfieldMan::isAllBitSet() const {
  if(bitfieldLength == 0) {
    return true;
  }
  for(size_t i = 0; i < bitfieldLength-1; ++i) {
    if(bitfield[i] != 0xff) {
      return false;
    }
  }
  unsigned char b = ~((128 >> (blocks-1)%8)-1);
  if(bitfield[bitfieldLength-1] != b) {
    return false;
  }
  return true;
}

bool BitfieldMan::isBitSet(size_t index) const
{
  return bitfield::test(bitfield, blocks, index);
}

bool BitfieldMan::isUseBitSet(size_t index) const
{
  return bitfield::test(useBitfield, blocks, index);
}

void BitfieldMan::setBitfield(const unsigned char* bitfield, size_t bitfieldLength) {
  if(this->bitfieldLength != bitfieldLength) {
    return;
  }
  memcpy(this->bitfield, bitfield, this->bitfieldLength);
  memset(this->useBitfield, 0, this->bitfieldLength);
  updateCache();
}

const unsigned char* BitfieldMan::getBitfield() const
{
  return bitfield;
}

size_t BitfieldMan::getBitfieldLength() const
{
  return bitfieldLength;
}

void BitfieldMan::clearAllBit() {
  memset(this->bitfield, 0, this->bitfieldLength);
  updateCache();
}

void BitfieldMan::setAllBit() {
  for(size_t i = 0; i < blocks; ++i) {
    setBitInternal(bitfield, i, true);
  }
  updateCache();
}

void BitfieldMan::clearAllUseBit() {
  memset(this->useBitfield, 0, this->bitfieldLength);
  updateCache();
}

void BitfieldMan::setAllUseBit() {
  for(size_t i = 0; i < blocks; ++i) {
    setBitInternal(useBitfield, i, true);
  }
}

bool BitfieldMan::setFilterBit(size_t index) {
  return setBitInternal(filterBitfield, index, true);
}

void BitfieldMan::addFilter(uint64_t offset, uint64_t length) {
  if(!filterBitfield) {
    filterBitfield = new unsigned char[bitfieldLength];
    memset(filterBitfield, 0, bitfieldLength);
  }
  if(length > 0) {
    size_t startBlock = offset/blockLength;
    size_t endBlock = (offset+length-1)/blockLength;
    for(size_t i = startBlock; i <= endBlock && i < blocks; i++) {
      setFilterBit(i);
    }
  }
  updateCache();
}

void BitfieldMan::enableFilter() {
  filterEnabled = true;
  updateCache();
}

void BitfieldMan::disableFilter() {
  filterEnabled = false;
  updateCache();
}

void BitfieldMan::clearFilter() {
  if(filterBitfield) {
    delete [] filterBitfield;
    filterBitfield = 0;
  }
  filterEnabled = false;
  updateCache();
}

bool BitfieldMan::isFilterEnabled() const {
  return filterEnabled;
}

uint64_t BitfieldMan::getFilteredTotalLength() const {
  return cachedFilteredTotalLength;
}

uint64_t BitfieldMan::getFilteredTotalLengthNow() const {
  if(!filterBitfield) {
    return 0;
  }
  size_t filteredBlocks = bitfield::countSetBit(filterBitfield, blocks);
  if(filteredBlocks == 0) {
    return 0;
  }
  if(bitfield::test(filterBitfield, blocks, blocks-1)) {
    return ((uint64_t)filteredBlocks-1)*blockLength+getLastBlockLength();
  } else {
    return ((uint64_t)filteredBlocks)*blockLength;
  }
}

uint64_t BitfieldMan::getCompletedLength(bool useFilter) const {
  unsigned char* temp = new unsigned char[bitfieldLength];
  if(useFilter) {
    for(size_t i = 0; i < bitfieldLength; ++i) {
      temp[i] = bitfield[i];
      if(filterEnabled) {
	temp[i] &= filterBitfield[i];
      }
    }
  } else {
    memcpy(temp, bitfield, bitfieldLength);
  }
  size_t completedBlocks = bitfield::countSetBit(temp, blocks);
  uint64_t completedLength = 0;
  if(completedBlocks == 0) {
    completedLength = 0;
  } else {
    if(bitfield::test(temp, blocks, blocks-1)) {
      completedLength = ((uint64_t)completedBlocks-1)*blockLength+getLastBlockLength();
    } else {
      completedLength = ((uint64_t)completedBlocks)*blockLength;
    }
  }
  delete [] temp;
  return completedLength;
}

uint64_t BitfieldMan::getCompletedLength() const {
  return cachedCompletedLength;
}

uint64_t BitfieldMan::getCompletedLengthNow() const {
  return getCompletedLength(false);
}

uint64_t BitfieldMan::getFilteredCompletedLength() const {
  return cachedFilteredComletedLength;
}

uint64_t BitfieldMan::getFilteredCompletedLengthNow() const {
  return getCompletedLength(true);
}

void BitfieldMan::updateCache()
{
  cachedNumMissingBlock = countMissingBlockNow();
  cachedNumFilteredBlock = countFilteredBlockNow();
  cachedFilteredTotalLength = getFilteredTotalLengthNow();
  cachedCompletedLength = getCompletedLengthNow();
  cachedFilteredComletedLength = getFilteredCompletedLengthNow();
}

bool BitfieldMan::isBitRangeSet(size_t startIndex, size_t endIndex) const
{
  for(size_t i =  startIndex; i <= endIndex; ++i) {
    if(!isBitSet(i)) {
      return false;
    }
  }
  return true;
}

void BitfieldMan::unsetBitRange(size_t startIndex, size_t endIndex)
{
  for(size_t i = startIndex; i <= endIndex; ++i) {
    unsetBit(i);
  }
  updateCache();
}

void BitfieldMan::setBitRange(size_t startIndex, size_t endIndex)
{
  for(size_t i = startIndex; i <= endIndex; ++i) {
    setBit(i);
  }
  updateCache();
}

bool BitfieldMan::isBitSetOffsetRange(uint64_t offset, uint64_t length) const
{
  if(length <= 0) {
    return false;
  }
  if(totalLength <= offset) {
    return false;
  }
  if(totalLength < offset+length) {
    length = totalLength-offset;
  }
  size_t startBlock = offset/blockLength;
  size_t endBlock = (offset+length-1)/blockLength;
  for(size_t i = startBlock; i <= endBlock; i++) {
    if(!isBitSet(i)) {
      return false;
    }
  }
  return true;
}

uint64_t BitfieldMan::getMissingUnusedLength(size_t startingIndex) const
{
  if(startingIndex < 0 || blocks <= startingIndex) {
    return 0;
  }
  uint64_t length = 0;
  for(size_t i = startingIndex; i < blocks; ++i) {
    if(isBitSet(i) || isUseBitSet(i)) {
      break;
    }
    length += getBlockLength(i);
  }
  return length;
}

void BitfieldMan::setRandomizer(const SharedHandle<Randomizer>& randomizer)
{
  this->randomizer = randomizer;
}

SharedHandle<Randomizer> BitfieldMan::getRandomizer() const
{
  return randomizer;
}

} // namespace aria2
