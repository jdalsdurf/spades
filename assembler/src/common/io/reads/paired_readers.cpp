//***************************************************************************
//* Copyright (c) 2019 Saint Petersburg State University
//* All Rights Reserved
//* See file LICENSE for details.
//***************************************************************************

#include "paired_readers.hpp"

#include "file_reader.hpp"
#include "async_read_stream.hpp"

#include "utils/logger/logger.hpp"

#include <string>

namespace io {

SeparatePairedReadStream::SeparatePairedReadStream(const std::string& filename1, const std::string& filename2,
                                                   size_t insert_size,
                                                   OffsetType offset_type)
        : insert_size_(insert_size),
          first_(make_async_stream<FileReadStream>(filename1, offset_type)),
          second_(make_async_stream<FileReadStream>(filename2, offset_type)),
          filename1_(filename1),
          filename2_(filename2)
{}

bool SeparatePairedReadStream::eof() {
    if (first_->eof() != second_->eof()) {
        if (first_->eof()) {
            ERROR("The number of right read-pairs is larger than the number of left read-pairs");
        } else {
            ERROR("The number of left read-pairs is larger than the number of right read-pairs");
        }
        FATAL_ERROR("Unequal number of read-pairs detected in the following files: " << filename1_ << "  " << filename2_ << "");
    }
    return first_->eof();
  }

SeparatePairedReadStream& SeparatePairedReadStream::operator>>(PairedRead& pairedread) {
    pairedread.set_orig_insert_size(insert_size_);
    (*first_) >> pairedread.first();
    (*second_) >> pairedread.second();
    return *this;
}

InterleavingPairedReadStream::InterleavingPairedReadStream(const std::string& filename,
                                                           size_t insert_size,
                                                           OffsetType offset_type)
        : filename_(filename), insert_size_(insert_size),
          single_(make_async_stream<FileReadStream>(filename_, offset_type)) {}

InterleavingPairedReadStream& InterleavingPairedReadStream::operator>>(PairedRead& pairedread) {
    pairedread.set_orig_insert_size(insert_size_);
    (*single_) >> pairedread.first();
    (*single_) >> pairedread.second();
    return *this;
}

}
