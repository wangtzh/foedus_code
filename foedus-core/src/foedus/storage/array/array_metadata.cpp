/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#include <foedus/externalize/externalizable.hpp>
#include <foedus/storage/array/array_metadata.hpp>
namespace foedus {
namespace storage {
namespace array {
ErrorStack ArrayMetadata::load(tinyxml2::XMLElement* element) {
    CHECK_ERROR(load_base(element));
    EXTERNALIZE_LOAD_ELEMENT(element, payload_size_);
    EXTERNALIZE_LOAD_ELEMENT(element, array_size_);
    EXTERNALIZE_LOAD_ELEMENT(element, root_page_id_);
    return RET_OK;
}

ErrorStack ArrayMetadata::save(tinyxml2::XMLElement* element) const {
    CHECK_ERROR(save_base(element));
    EXTERNALIZE_SAVE_ELEMENT(element, payload_size_, "");
    EXTERNALIZE_SAVE_ELEMENT(element, array_size_, "");
    EXTERNALIZE_SAVE_ELEMENT(element, root_page_id_, "");
    return RET_OK;
}

Metadata* ArrayMetadata::clone() const {
    ArrayMetadata* cloned = new ArrayMetadata();
    clone_base(cloned);
    cloned->payload_size_ = payload_size_;
    cloned->array_size_ = array_size_;
    cloned->root_page_id_ = root_page_id_;
    return cloned;
}


}  // namespace array
}  // namespace storage
}  // namespace foedus
