#include "internal.h"

int tudor_wipe_records(struct tudor_device *device, RECGUID *guid, enum tudor_finger finger) {
    cant_fail_ret(pthread_mutex_lock(&device->records_lock));

    //Find the record
    int num_deleted = 0;
    for(struct tudor_record *rec = device->records_head, *nrec = rec ? rec->next : NULL; rec; rec = nrec, nrec = rec ? rec->next : NULL) {
        if(
            (guid == NULL || memcmp(&rec->identity->TemplateGuid, guid, sizeof(GUID)) == 0) && 
            (finger == TUDOR_FINGER_ANY || rec->finger == finger)
        ) {
            //Remove from record list
            if(rec->prev) rec->prev->next = rec->next;
            else device->records_head = rec->next;
            if(rec->next) rec->next->prev = rec->prev;
            free(rec->data);
            free(rec);

            device->result_records_head = device->result_records_cursor = NULL;
            num_deleted++;
        }
    }

    cant_fail_ret(pthread_mutex_unlock(&device->records_lock));
    return num_deleted;
}

bool tudor_add_record(struct tudor_device *device, RECGUID guid, enum tudor_finger finger, const void *data, size_t data_size) {
    cant_fail_ret(pthread_mutex_lock(&device->records_lock));

    //Check for duplicate record
    for(struct tudor_record *rec = device->records_head; rec; rec = rec->next) {
        if(memcmp(&rec->guid, &guid, sizeof(RECGUID)) == 0 && rec->finger == finger) {
            cant_fail_ret(pthread_mutex_unlock(&device->records_lock));
            return false;
        }
    }

    //Create record
    struct tudor_record *rec = malloc(sizeof(struct tudor_record));
    if(!rec) { perror("Couldn't allocate record"); abort(); }

    rec->identity = (WINBIO_IDENTITY*) malloc(sizeof(WINBIO_IDENTITY));
    if(!rec->identity) { perror("Couldn't allocate record identity"); abort(); }
    rec->identity->Type = WINBIO_ID_TYPE_GUID;
    rec->identity->TemplateGuid = *(GUID*) (void*) &guid;

    rec->guid  = guid;
    rec->finger = finger;
    rec->data = malloc(data_size);
    rec->data_size = data_size;
    if(!rec->data)  { perror("Couldn't allocate record data"); abort(); }
    memcpy(rec->data, data, data_size);

    //Add to record list
    rec->prev = NULL;
    rec->next = device->records_head;
    if(device->records_head) device->records_head->prev = rec;
    device->records_head = rec;

    cant_fail_ret(pthread_mutex_unlock(&device->records_lock));
    return true;
}

__winfnc static HRESULT storage_NOTIMPL() {
    log_error("Not implemented storage adapter function called!");
    abort();
}

__winfnc static HRESULT storage_NOP() {
    return ERROR_SUCCESS;
}

//Named logging NOPs so we can see exactly which storage adapter slots the DLL
//calls (the bare storage_NOP is shared across ~11 vtable slots and is invisible).
#define STORAGE_LOG_NOP(name) __winfnc static HRESULT storage_log_##name() { log_verbose("WINBIO storage | " #name "() called"); return ERROR_SUCCESS; }
STORAGE_LOG_NOP(Attach)
STORAGE_LOG_NOP(Detach)
STORAGE_LOG_NOP(ClearContext)
STORAGE_LOG_NOP(NotifyPowerChange)
STORAGE_LOG_NOP(PipelineInit)
STORAGE_LOG_NOP(PipelineCleanup)
STORAGE_LOG_NOP(Activate)
STORAGE_LOG_NOP(Deactivate)
STORAGE_LOG_NOP(NotifyDatabaseChange)
STORAGE_LOG_NOP(UpdateRecordBegin)
STORAGE_LOG_NOP(UpdateRecordCommit)

__winfnc static HRESULT storage_ControlUnit() {
    log_verbose("WINBIO storage | ControlUnit() called");
    return E_INVALIDARG;
}

__winfnc static HRESULT storage_QueryExtendedInfo(WINBIO_PIPELINE *pipeline, WINBIO_EXTENDED_STORAGE_INFO *info, SIZE_T info_size) {
    log_verbose("WINBIO storage | QueryExtendedInfo(info_size=%lu) called", info_size);
    if(sizeof(WINBIO_EXTENDED_STORAGE_INFO) > info_size) return E_INVALIDARG;
    info->GenericStorageCapabilities = WINBIO_CAPABILITY_MATCHING;
    info->Factor = WINBIO_TYPE_FINGERPRINT;
    info->Specific.Fingerprint.Capabilities = 0;
    return ERROR_SUCCESS;
}

__winfnc static HRESULT storage_AddRecord(WINBIO_PIPELINE *pipeline, WINBIO_STORAGE_RECORD *srec) {
    struct tudor_device *dev = (struct tudor_device*) pipeline->StorageContext;

    //DIAGNOSTIC: dump exactly what the DLL passes us, BEFORE any validation, so we
    //can see whether it hands us a real on-device template handle as TemplateBlob.
    if(LOG_LEVEL <= LOG_VERBOSE) {
        cant_fail_ret(pthread_mutex_lock(&LOG_LOCK));
        printf("[ADDREC] guid=%08x subfactor=%x idxcount=%u TemplateBlobSize=%lu Template=",
            srec->Identity->TemplateGuid.PartA, srec->SubFactor, srec->IndexElementCount, srec->TemplateBlobSize);
        if(srec->TemplateBlob && srec->TemplateBlobSize) { for(SIZE_T i = 0; i < srec->TemplateBlobSize; i++) printf("%02x", srec->TemplateBlob[i]); }
        else printf("(none)");
        printf(" PayloadBlobSize=%lu Payload=", srec->PayloadBlobSize);
        if(srec->PayloadBlob && srec->PayloadBlobSize) { for(SIZE_T i = 0; i < srec->PayloadBlobSize; i++) printf("%02x", srec->PayloadBlob[i]); }
        else printf("(none)");
        puts("");
        cant_fail_ret(pthread_mutex_unlock(&LOG_LOCK));
    }

    //Validate record
    if(srec->Identity->Type != WINBIO_ID_TYPE_GUID) return E_INVALIDARG;
    if(srec->IndexElementCount != 0 || srec->PayloadBlobSize != 0) return E_INVALIDARG;

    log_verbose("WINBIO storage | AddRecord(guid=%08x... subfactor=%x) called", srec->Identity->TemplateGuid.PartA, srec->SubFactor);
    cant_fail_ret(pthread_mutex_lock(&dev->records_lock));

    //Check for duplicate record
    for(struct tudor_record *rec = dev->records_head; rec; rec = rec->next) {
        if(memcmp(&rec->identity->TemplateGuid, &srec->Identity->TemplateGuid, sizeof(GUID)) == 0 && rec->finger == srec->SubFactor) {
            cant_fail_ret(pthread_mutex_unlock(&dev->records_lock));
            return WINBIO_E_DUPLICATE_ENROLLMENT;
        }
    }

    //Add new record
    struct tudor_record *rec = (struct tudor_record*) malloc(sizeof(struct tudor_record));
    if(!rec) {
        HRESULT hr = winerr_from_errno();
        cant_fail_ret(pthread_mutex_unlock(&dev->records_lock));
        return hr;
    }

    rec->prev = NULL;
    rec->next = dev->records_head;

    rec->identity = (WINBIO_IDENTITY*) malloc(sizeof(WINBIO_IDENTITY));
    if(!rec->identity) {
        HRESULT hr = winerr_from_errno();
        cant_fail_ret(pthread_mutex_unlock(&dev->records_lock));
        return hr;
    }
    *rec->identity = *srec->Identity;

    rec->guid = *(RECGUID*) &srec->Identity->TemplateGuid;
    rec->finger = (enum tudor_finger) srec->SubFactor;

    // For on-device storage (Prometheus), the DLL provides TemplateBlobSize=0.
    // Store a 1-byte placeholder so IdentifyFeatureSet includes this record's
    // GUID in the 0x442058 identify payload (count=0 causes "no match").
    size_t store_size = srec->TemplateBlobSize > 0 ? srec->TemplateBlobSize : 1;
    rec->data = malloc(store_size);
    rec->data_size = store_size;
    if(!rec->data) {
        HRESULT hr = winerr_from_errno();
        free(rec);
        cant_fail_ret(pthread_mutex_unlock(&dev->records_lock));
        return hr;
    }
    memset(rec->data, 0, store_size);
    if(srec->TemplateBlobSize > 0) memcpy(rec->data, srec->TemplateBlob, srec->TemplateBlobSize);

    if(dev->records_head) dev->records_head->prev = rec;
    dev->records_head = rec;

    cant_fail_ret(pthread_mutex_unlock(&dev->records_lock));

    //Register the on-device template under this identity (IOCTL 0x442018) so the
    //on-chip matcher can find it. The DLL's native StorageAdapterAddRecord does
    //this; the host-only adapter historically didn't, causing identify/verify to
    //return NO_RESULTS. Done outside records_lock (it talks to the device).
    if(!tudor_register_ondevice_template(dev, srec->Identity, srec->SubFactor, srec->TemplateBlob, srec->TemplateBlobSize)) {
        log_warn("Couldn't register template on-device; identify may not match this finger");
    }

    return ERROR_SUCCESS;
}

__winfnc static HRESULT storage_DeleteRecord(WINBIO_PIPELINE *pipeline, WINBIO_IDENTITY *ident, UCHAR subfactor) {
    struct tudor_device *dev = (struct tudor_device*) pipeline->StorageContext;

    //Validate identity
    if(ident->Type != WINBIO_ID_TYPE_WILDCARD && ident->Type != WINBIO_ID_TYPE_GUID) return E_INVALIDARG;

    log_verbose("WINBIO storage | DeleteRecord(ID type=%d guid=%08x... subfactor=%x) called", ident->Type, ident->TemplateGuid.PartA, subfactor);
    cant_fail_ret(pthread_mutex_lock(&dev->records_lock));

    //Find the record
    bool any_deleted = false;
    for(struct tudor_record *rec = dev->records_head, *nrec = rec ? rec->next : NULL; rec; rec = nrec, nrec = rec ? rec->next : NULL) {
        if(
            (ident->Type == WINBIO_ID_TYPE_WILDCARD || memcmp(&rec->identity->TemplateGuid, &ident->TemplateGuid, sizeof(GUID)) == 0) && 
            (subfactor == WINBIO_SUBTYPE_ANY || rec->finger == subfactor)
        ) {
            //Remove from record list
            if(rec->prev) rec->prev->next = rec->next;
            else dev->records_head = rec->next;
            if(rec->next) rec->next->prev = rec->prev;
            free(rec->data);
            free(rec->identity);
            free(rec);

            dev->result_records_head = dev->result_records_cursor = NULL;
            any_deleted = true;
        }
    }

    cant_fail_ret(pthread_mutex_unlock(&dev->records_lock));
    return any_deleted ? ERROR_SUCCESS :  WINBIO_E_DATABASE_NO_SUCH_RECORD;
}

__winfnc static HRESULT storage_QueryBySubject(WINBIO_PIPELINE *pipeline, WINBIO_IDENTITY *ident, UCHAR subfactor) {
    struct tudor_device *dev = (struct tudor_device*) pipeline->StorageContext;

    //Validate identity
    if(ident->Type != WINBIO_ID_TYPE_WILDCARD && ident->Type != WINBIO_ID_TYPE_GUID) return E_INVALIDARG;

    log_verbose("WINBIO storage | QueryBySubject(ID type=%d guid=%08x... subfactor=%x) called", ident->Type, ident->TemplateGuid.PartA, subfactor);
    cant_fail_ret(pthread_mutex_lock(&dev->records_lock));

    //Find the record
    struct tudor_record *prev_res = NULL;
    dev->result_records_head = dev->result_records_cursor = NULL;
    for(struct tudor_record *rec = dev->records_head; rec; rec = rec->next) {
        if(
            (ident->Type == WINBIO_ID_TYPE_WILDCARD || memcmp(&rec->identity->TemplateGuid, &ident->TemplateGuid, sizeof(GUID)) == 0) && 
            (subfactor == WINBIO_SUBTYPE_ANY || rec->finger == subfactor)
        ) {
            log_verbose("WINBIO storage | QueryBySubject | Found record! guid=%08x... finger=%x", rec->identity->TemplateGuid.PartA, rec->finger);

            //Add to result records
            if(prev_res) prev_res->res_next = rec;
            else dev->result_records_head = dev->result_records_cursor = rec;
            rec->res_next = NULL;
            prev_res = rec;
        }
    }

    cant_fail_ret(pthread_mutex_unlock(&dev->records_lock));
    return prev_res ? ERROR_SUCCESS : WINBIO_E_DATABASE_NO_RESULTS;
}

__winfnc static HRESULT storage_QueryByContent(WINBIO_PIPELINE *pipeline, UCHAR subfactor, ULONG index_vec[], SIZE_T index_vec_sz) {
    //Validate index vector
    if(index_vec_sz != 0) return E_INVALIDARG;

    //Forward to QueryBySubject
    log_verbose("WINBIO storage | QueryByContent(subfactor=%x) called", subfactor);
    return storage_QueryBySubject(pipeline, &(WINBIO_IDENTITY) {
        .Type = WINBIO_ID_TYPE_WILDCARD,
        .Null = 0
    }, subfactor);
}

__winfnc static HRESULT storage_GetRecordCount(WINBIO_PIPELINE *pipeline, SIZE_T *num_recs) {
    struct tudor_device *dev = (struct tudor_device*) pipeline->StorageContext;

    cant_fail_ret(pthread_mutex_lock(&dev->records_lock));

    //Follow result chain
    *num_recs = 0;
    for(struct tudor_record *rec = dev->result_records_head; rec; rec = rec->res_next) (*num_recs)++;

    cant_fail_ret(pthread_mutex_unlock(&dev->records_lock));
    log_verbose("WINBIO storage | GetRecordCount() called -> %lu records", *num_recs);
    return *num_recs ? ERROR_SUCCESS : WINBIO_E_DATABASE_NO_RESULTS;
}

__winfnc static HRESULT storage_FirstRecord(WINBIO_PIPELINE *pipeline) {
    struct tudor_device *dev = (struct tudor_device*) pipeline->StorageContext;

    log_verbose("WINBIO storage | FirstRecord() called");
    cant_fail_ret(pthread_mutex_lock(&dev->records_lock));

    if(!dev->result_records_head) {
        cant_fail_ret(pthread_mutex_unlock(&dev->records_lock));
        return WINBIO_E_DATABASE_NO_RESULTS;
    }

    dev->result_records_cursor = dev->result_records_head;
    
    cant_fail_ret(pthread_mutex_unlock(&dev->records_lock));
    return ERROR_SUCCESS;
}

__winfnc static HRESULT storage_NextRecord(WINBIO_PIPELINE *pipeline) {
    struct tudor_device *dev = (struct tudor_device*) pipeline->StorageContext;

    log_verbose("WINBIO storage | NextRecord() called");
    cant_fail_ret(pthread_mutex_lock(&dev->records_lock));

    if(!dev->result_records_head) {
        cant_fail_ret(pthread_mutex_unlock(&dev->records_lock));
        return WINBIO_E_DATABASE_NO_RESULTS;
    }

    struct tudor_record *ncursor = (dev->result_records_cursor ? dev->result_records_cursor->res_next : NULL);
    if(ncursor) dev->result_records_cursor = ncursor;

    cant_fail_ret(pthread_mutex_unlock(&dev->records_lock));
    return ncursor ? ERROR_SUCCESS : WINBIO_E_DATABASE_NO_MORE_RECORDS;
}

__winfnc static HRESULT storage_GetCurrentRecord(WINBIO_PIPELINE *pipeline, WINBIO_STORAGE_RECORD *srec) {
    struct tudor_device *dev = (struct tudor_device*) pipeline->StorageContext;

    log_verbose("WINBIO storage | GetCurrentRecord() called");
    cant_fail_ret(pthread_mutex_lock(&dev->records_lock));

    if(!dev->result_records_head) {
        cant_fail_ret(pthread_mutex_unlock(&dev->records_lock));
        return WINBIO_E_DATABASE_NO_RESULTS;
    }

    srec->Identity = dev->result_records_cursor->identity;
    srec->SubFactor = (UCHAR) dev->result_records_cursor->finger;
    srec->IndexVector = NULL;
    srec->IndexElementCount = 0;
    srec->TemplateBlob = dev->result_records_cursor->data;
    srec->TemplateBlobSize = dev->result_records_cursor->data_size;
    srec->PayloadBlob = NULL;
    srec->PayloadBlobSize = 0;

    cant_fail_ret(pthread_mutex_unlock(&dev->records_lock));
    return ERROR_SUCCESS;
}

WINBIO_STORAGE_INTERFACE *tudor_storage_adapter = &(WINBIO_STORAGE_INTERFACE) {
    .Version = { 5, 0 },
    .Type = 3,
    .Size = sizeof(WINBIO_STORAGE_INTERFACE),
    .Attach = (void*) storage_log_Attach,
    .Detach = (void*) storage_log_Detach,
    .ClearContext = (void*) storage_log_ClearContext,
    .CreateDatabase = storage_NOTIMPL,
    .EraseDatabase = storage_NOTIMPL,
    .OpenDatabase = storage_NOTIMPL,
    .CloseDatabase = storage_NOTIMPL,
    .GetDataFormat = storage_NOTIMPL,
    .GetDatabaseSize = storage_NOTIMPL,
    .AddRecord = storage_AddRecord,
    .DeleteRecord = storage_DeleteRecord,
    .QueryBySubject = storage_QueryBySubject,
    .QueryByContent = storage_QueryByContent,
    .GetRecordCount = storage_GetRecordCount,
    .FirstRecord = storage_FirstRecord,
    .NextRecord = storage_NextRecord,
    .GetCurrentRecord = storage_GetCurrentRecord,
    .ControlUnit = storage_ControlUnit,
    .ControlUnitPrivileged = storage_ControlUnit,
    .NotifyPowerChange = (IBIO_SENSOR_NOTIFY_POWER_CHANGE_FN*) storage_log_NotifyPowerChange,
    .PipelineInit = (void*) storage_log_PipelineInit,
    .PipelineCleanup = (void*) storage_log_PipelineCleanup,
    .Activate = (void*) storage_log_Activate,
    .Deactivate = (void*) storage_log_Deactivate,
    .QueryExtendedInfo = storage_QueryExtendedInfo,
    .NotifyDatabaseChange = (IBIO_STORAGE_NOTIFY_DATABASE_CHANGE_FN*) storage_log_NotifyDatabaseChange,
    .GetUserState = NULL, //Reserved1
    .IncrementUserState = NULL, //Reserved2
    .UpdateRecordBegin = (IBIO_STORAGE_UPDATE_RECORD_BEGIN_FN*) storage_log_UpdateRecordBegin,
    .UpdateRecordCommit = (IBIO_STORAGE_UPDATE_RECORD_COMMIT_FN*) storage_log_UpdateRecordCommit
};