/*
 * kvat.c
 *
 *      Author: repixen
 */

#include <stdlib.h>
#include <string.h>
#include <driverlib/sysctl.h>
#include <driverlib/eeprom.h>
#include <kvat/kvat.h>

//==========================================================
// FORMATTING LIMITS

#define FORMATID 203
#define PAGESIZE 8      // Pages need to be a multiple of 4 in size (max 256)
#define PAGECOUNT 128   // 255 max on a single byte paging scheme

//==========================================================
// GENERAL LIMITS

#define INDEXSTART 0    // Address that the index starts on in storage

//==========================================================
// RECOMMENDED LIMITS

#define STRINGKEYSTDLEN 16  // Expected maximum length for string-keys (baseline, not enforced)

//==========================================================
/* TABLE ENTRY METADATA FORMATTING
 *
 * x x KF KF  VT KT ST ST > lsb
 *
 */

// STATUS
#define MACTIVE      0x01    // (bool) Active entry (currently pointing to valid chains)
#define MOPEN        0x02    // (bool) Entry is currently being edited

// KEY TYPE
#define MKEYCHAIN    0x04    // Mask
#define MKC_MULTIPLE 0x00    // Key is stored in multiple pages
#define MKC_SINGLE   0x04    // Key is stored in single page

// VALUE
#define MVALUECHAIN  0x08    // Mask
#define MVC_MULTIPLE 0x00    // Value is stored in multiple pages
#define MVC_SINGLE   0x08    // Value is stored in single page

// KEY FORMAT
#define MKEYFORMAT   0x30    // Mask
#define MKF_STRING   0x00    // String
#define MKF_UINT32   0x10    // Unsigned int
//#define MKF_         0x20    // (undefined)
//#define MKF_         0x30    // (undefined)



//==========================================================

typedef unsigned char MetaData;
typedef unsigned char PageNumber;
typedef uint32_t StorageAddress;
typedef uint32_t PageData;
typedef uint32_t* PageDataRef;

//==========================================================

bool isInit = false;
unsigned char* pageRecord = NULL;

// Structs need to be a multiple of 4 bytes in size!!

// Multiple of 4 by design
typedef struct KVATKeyValueEntry{
    MetaData metadata;
    PageNumber keyPage;
    PageNumber valuePage;
    unsigned char remains; // Number of bytes that the value should be truncated from max page-chain (data) size
}KVATKeyValueEntry;

// Multiple of 4 by alignment
// The table is part of the index, but never loaded or saved from storage entirely. Entries from table should be handled individually.
typedef struct KVATIndex{
    uint16_t formatID;
    KVATSize pageSize;
    PageNumber pageCount;
    StorageAddress pageBeginAddress;   // Since this is 4 byte aligned, the table (next) will be as well
    //KVATKeyValueEntry table[PAGECOUNT];
}KVATIndex;

static KVATIndex* index = NULL;

static bool saveIndex(){

    // Produce a copy of the index to store
    uint32_t* indexCopy = malloc(sizeof(KVATIndex));
    memcpy(indexCopy, index, sizeof(KVATIndex));

    uint32_t programResult = EEPROMProgram(indexCopy, INDEXSTART, sizeof(KVATIndex));

    // Free space from the copy
    free(indexCopy);

    return !programResult;
}

static void readIndex(){
    // Read into compatible uint32_t buffer
    uint32_t* indexBuff = malloc(sizeof(KVATIndex));
    EEPROMRead(indexBuff, INDEXSTART, sizeof(KVATIndex));

    // Copy into actual index
    if (index!=NULL){
        memcpy(index, indexBuff, sizeof(KVATIndex));
    }

    // Get rid of buffer
    free(indexBuff);
}

static void setEntryMetadata(KVATKeyValueEntry* entry, MetaData mask, MetaData value){
    entry->metadata &= ~mask;         // Clear position
    entry->metadata |= value & mask;  // Set value
}

static StorageAddress getEntryAddressFromPosition(PageNumber entryPosition){
    return INDEXSTART + sizeof(KVATIndex) + sizeof(KVATKeyValueEntry)*entryPosition;
}

/**
 * Writes only the section of the index pertaining to a single table entry into storage.
 *
 * @param      entryToSave          Reference to a KVATKeyValueEntry instance to save
 * @param      entryPosition        The position to save in the entry table.
 *
 * @return Success of the save process. True if successful.
 */
static bool saveTableEntry(KVATKeyValueEntry* entryToSave, PageNumber entryPosition){
    // Copy table entry into compatible uint32_t pointer
    uint32_t* entryCopy = malloc(sizeof(KVATKeyValueEntry));
    memcpy(entryCopy, entryToSave, sizeof(KVATKeyValueEntry));

    // Get address of the table entry position to save in
    StorageAddress entryAddress = getEntryAddressFromPosition(entryPosition);

    // Program the entry into storage
    uint32_t programResult = EEPROMProgram(entryCopy, entryAddress, sizeof(KVATKeyValueEntry));

    // Get rid of the memory for copy
    free(entryCopy);

    return !programResult;
}

static void readTableEntry(KVATKeyValueEntry* entryRead, PageNumber entryPosition){
    // Prepare buffer to read into
    uint32_t* entryBuff = malloc(sizeof(KVATKeyValueEntry));

    // Get address of the table entry to read
    StorageAddress entryAddress = getEntryAddressFromPosition(entryPosition);

    // Read entry from storage
    EEPROMRead(entryBuff, entryAddress, sizeof(KVATKeyValueEntry));

    // Copy read data into the right place
    memcpy(entryRead, entryBuff, sizeof(KVATKeyValueEntry));

    // Get rid of the read buffer
    free(entryBuff);
}

/**
 * Returns the number in the index table of an empty entry spot.
 * Note: 0 is reserved for invalid page.
 *
 * @return Number of the empty entry, or 0 if all full.
 */
static PageNumber getEmptyTableEntryNumber(){
    // Get some local references of page count (algo number of table entries)
    PageNumber entryCount = index->pageCount;

    KVATKeyValueEntry entry;

    // Start checkin'
    for (PageNumber entryN = 1; entryN<entryCount; entryN++){
        readTableEntry(&entry, entryN);

        if (!(entry.metadata & (MACTIVE | MOPEN))){ // Check status to see if actually empty
            return entryN;
        }
    }
    return 0;
}

/**
 * Calculates and returns the address of page 0 based on the definitions for INDEXSTART, PAGECOUNT & KVAT structs.
 * Warning: this is intended for formatting calculation. If already formatted, get from format settings (in index).
 *
 * @return Address of page 0 in storage.
 */
static StorageAddress getNaturalAddressOfPage0(){
    return INDEXSTART + sizeof(KVATIndex) + sizeof(KVATKeyValueEntry)*PAGECOUNT;
}

/**
 * Formats storage based on defined formatting limits.
 * (Writes empty index)
 * Should only be called by an init or a reformat operation.
 *
 * @return boolean of operation result. true on success.
 */
static bool formatMemory(){
    //GUARD - no formatting if initialized
    if (isInit){return false;}

    //Prepare index with formatting limits and paging region
    index->formatID = FORMATID;
    index->pageSize = PAGESIZE;
    index->pageCount = PAGECOUNT;
    index->pageBeginAddress = getNaturalAddressOfPage0();

    KVATKeyValueEntry emptyEntry = {.metadata = 0};

    //Mark entries as empty (including invalid page 0)
    for (PageNumber entryN = 0; entryN<PAGECOUNT; entryN++){
        saveTableEntry(&emptyEntry, entryN);
    }

    return saveIndex();
}

/**
 * Converts the number of a page into the address it should hold in storage.
 *
 * @param      pageNumber        Number of a page to convert to address.
 *
 * @return Address of the page in storage based on page region address and page size.
 */
static StorageAddress getPageAddress(PageNumber pageNumber){
    if (pageNumber==0){return 0;}

    // Convert page number into relative address
    StorageAddress pageAddress = pageNumber*index->pageSize;

    // Offset into absolute address
    pageAddress += index->pageBeginAddress;

    return pageAddress;
}

/**
 * Reads page from storage into a buffer.
 *
 * @param[out] pageData        Reference to a buffer that will be used to dump the page data
 * @param      pageNumber      Number of the page to read.
 * @param      limitReadSize   Optional: Number of bytes to limit the reading to. Pass 0 to read entire length of page.
 */
static void readPage(PageDataRef pageData, PageNumber pageNumber, uint32_t limitReadSize){
    StorageAddress pageAddress = getPageAddress(pageNumber);

    // Read from address
    EEPROMRead(pageData, pageAddress, limitReadSize ? limitReadSize : index->pageSize);
}

/**
 * Writes page to storage from a buffer
 *
 * @param      pageData        Reference to a buffer containing the data of the page to write
 * @param      pageNumber      Number of the page to write to.
 *
 * @return Boolean with success of operation.
 */
static bool writePage(PageDataRef pageData, PageNumber pageNumber){
    StorageAddress pageAddress = getPageAddress(pageNumber);

    // Write to address
    uint32_t programResult = EEPROMProgram(pageData, pageAddress, index->pageSize);

    return !programResult;
}

/**
 * Obtains the next page from the one passed, and returns it.
 * Warning: Does not validate result. Pages do not contain metadata to validate on their own.
 *
 * @param      pageData        Reference to the data of a page.
 *
 * @return Number of the page that is next.
 */
static PageNumber getNextPageNumberFromPage(PageDataRef pageData){
    // Copy the page number block to an actual PageNumber using memcpy
    // Could just alias pageData, but it's illegal and can cause problems if using compiler optimizations.
    PageNumber nextPage;
    memcpy(&nextPage, pageData, sizeof(PageNumber));
    return nextPage;
}

/**
 * Reads page from storage, obtains the number of the next page, and returns it.
 * Warning: Does not validate result. Pages do not contain metadata to validate on their own.
 *
 * @param      pageNumber        Page to get the next of.
 *
 * @return Number of the page that is next.
 */
static PageNumber readNextPageNumber(PageNumber pageNumber){
    PageData pageData;  // A single instance of PageData can contain the data for next page
    readPage(&pageData, pageNumber, sizeof(PageData));

    return getNextPageNumberFromPage(&pageData);
}

//////////////////////////////////////////////////////////////////
//  SIZES

static KVATSize getPageNextSize(bool isSinglePage){
    return isSinglePage ? 0 : sizeof(PageNumber);
}


//////////////////////////////////////////////////////////////////
//  PAGE RECORD

/**
 * Returns the recommended size of the page record based on the number of pages in the format.
 *
 * @return Size in bytes.
 */
static KVATSize getPageRecordSize(){
    // Calculate record size (bytes) based on the number of pages. Each byte can hold record for 8 pages.
    return (index->pageCount/8)+1;
}

/**
 * Sets the status of a page in the runtime record.
 *
 * @param      pageNumber        The number of the page to set status of.
 * @param      isUsed            The status to set. true if used.
 */
static void markPageInRecord(PageNumber pageNumber, bool isUsed){
    if (pageRecord==NULL){return;}
    KVATSize recordSegment = pageNumber/8;
    char recordBit = pageNumber%8;

    if (isUsed){//Bitset
        pageRecord[recordSegment] |= 1<<recordBit;
    }else{
        //Bitclear
        pageRecord[recordSegment] &= ~(1<<recordBit);
    }
}

/**
 * Returns the status of a page based on the runtime record.
 *
 * @param      pageNumber        The number of the page to check
 *
 * @return true           If used
 * @return false          If empty
 */
static bool checkPageFromRecord(PageNumber pageNumber){
    if (pageRecord==NULL || pageNumber==0){return true;}
    KVATSize recordSegment = pageNumber/8;
    char recordBit = pageNumber%8;

    return pageRecord[recordSegment]&(1<<recordBit);
}

/**
 * Gets the number of an empty page in the system based on the runtime page record. Can also mark the empty page as used in record.
 *
 * @param      shouldMarkAsUsed        Indicator for the record marking. Pass true to also mark the page as used.
 *
 * @return Number of an empty page
 */
static PageNumber getEmptyPageNumber(bool shouldMarkAsUsed){
    KVATSize pageRecordSize = getPageRecordSize();
    // Check by bytes (faster)
    KVATSize recordSegment = 0;
    char recordBit = 0;
    for ( ; recordSegment<pageRecordSize; recordSegment++){
        // Check if not full
        if (pageRecord[recordSegment]!=0xFF){
            unsigned char openSegment = pageRecord[recordSegment];
            // Find the 0
            //Check lower nibble
            if ((openSegment|0xF0) != 0xFF){
                // Lower is a charm
                openSegment &= 0x0F;
            }else{
                // High is the way to go
                openSegment = openSegment>>4;
                recordBit = 4;
            }

            // Check remaining 4 bits
            if ((openSegment|0xC) != 0xF){
                // Lower is a charm
                openSegment &= 0x3;
            }else{
                // Go hi
                openSegment = openSegment>>2;
                recordBit+=2;
            }

            // Only 2 bits to go
            if ((openSegment|0x2) != 0x3){
                // Lowest wins. Nothing to do
            }else{
                // High takes the race
                recordBit+=1;
            }
            break;
        }
    }

    PageNumber emptyPageFound = recordSegment*8+recordBit;

    if (shouldMarkAsUsed && emptyPageFound){
        markPageInRecord(emptyPageFound, true);
    }

    return emptyPageFound;
}

/**
 * Finds all the pages being used by a data chain starting in a specific page and sets the record.
 *
 * @param      chainStart        Page number the chain starts in.
 * @param      isActive          Status used to set the pages of the chain as.
 * @param      isSingle          Indicates if a chain is single paged
 */
static void followPageChainAndSetPageRecord(PageNumber chainStart, bool isActive, bool isSingle){
    PageNumber currentPageN = chainStart;
    PageNumber chainPageN = 0;// Marks the number of the current page in the chain
    PageNumber pageCount = index->pageCount;

    while (currentPageN!=0 && chainPageN<pageCount){
        // Mark page in record
        markPageInRecord(currentPageN, isActive);

        if (isSingle){
            // There is no next page on single files
            currentPageN = 0;
        }else{
            // Get next page
            currentPageN = readNextPageNumber(currentPageN);
        }

        // Add to safe limiter
        chainPageN++;
    }
}

/**
 * Allocates space for pageRecord and traverses the tables to reflect the status of the pages.
 * Called during init process.
 */
static void updatePageRecord(){
    // Update might need more memory. If called after initial exploration, throwaway.
    if (pageRecord!=NULL){
        free(pageRecord);
    }

    // Get memory to hold the page record
    KVATSize pageRecordSize = getPageRecordSize();
    pageRecord = malloc(pageRecordSize);

    // Set to 0's (empty)
    memset(pageRecord, 0, pageRecordSize);

    // Set page 0 to used (reserved)
    markPageInRecord(0, true);

    // Keep the number of existing entries locally (table entries equals the number of available pages)
    PageNumber numberOfEntries = index->pageCount;
    KVATKeyValueEntry entry;

    // Go through all table entries (starting at 1)
    for (PageNumber entryN = 1; entryN<numberOfEntries; entryN++){
        readTableEntry(&entry, entryN);

        // Check if entry is active and follow chains for name and value to update records
        if (entry.metadata & MACTIVE){
            //Follow key
            followPageChainAndSetPageRecord(entry.keyPage, true, entry.metadata & MKEYCHAIN);
            //Follow value
            followPageChainAndSetPageRecord(entry.valuePage, true, entry.metadata & MVALUECHAIN);
        }
    }
}

//////////////////////////////////////////////////////////////////
//  FETCH

/**
 * Pulls entire data chain into a single allocated buffer and returns pointer. Null terminated.
 * If expecting to perform multiple fetches, a preallocated memory region can be used for the fetched data.
 *
 * @param      startPage           The number of the page that the data chain starts on.
 * @param      isSinglePage        The type of chain. Pass true for a single page chain.
 * @param[out] size                Optional: The maximum size of the data read (if it filled all pages exactly).
 * @param      preallocBuffer      Optional: Reference to memory region for fetched data dumping (recommended for repetitive fetching).
 *                                           Note: if fetched data does not fit in this buffer, a separate memory region will be allocated.
 * @param      preallocBufferSize  Optional: The size of the preallocated buffer.
 *
 * @return Pointer to allocated buffer, or preallocated buffer if used.
 */
static PageDataRef fetchData(PageNumber startPage, bool isSinglePage, KVATSize* maxSize, PageDataRef preallocBuffer, KVATSize preallocBufferSize){
    //Get total size of chain
    PageNumber pageCount = 1;
    PageNumber currentPageN = startPage;
    if (!isSinglePage){

        for (; pageCount<index->pageCount; pageCount++){
            currentPageN = readNextPageNumber(currentPageN);

            if (currentPageN == 0){
                break;
            }
        }
    }

    // Calculate page internal sizes (take into account the single page case)
    KVATSize pageNextSize = getPageNextSize(isSinglePage);
    KVATSize pageDataSize = index->pageSize-pageNextSize;
    KVATSize recordSize = pageDataSize*pageCount+1;         // Size of the record being fetched (plus 1 for null terminator)

    // Make nice buffers. One to keep a single page, another to keep the whole data read.
    PageDataRef singlePage = malloc(index->pageSize);
    PageDataRef record = (preallocBuffer!=NULL && preallocBufferSize>=recordSize) ? preallocBuffer : malloc(recordSize);

    // Add null terminator in extra byte
    record[pageDataSize*pageCount] = '\0';

    // Restart current page number
    currentPageN = startPage;

    // Fetch into buffer
    for (PageNumber i = 0; i<pageCount; i++){
        // Get the page (with next page pointer and all)
        readPage(singlePage, currentPageN, 0);

        // Only transfer data to nice record
        // Cast to char* [legal move] to do pointer arithmetic
        // (offset destination to fill the space of the current page)
        // (and offset source to jump over the next page segment)
        memcpy((char*)record+pageDataSize*i, (char*)singlePage+pageNextSize, pageDataSize);

        // Get next Page
        currentPageN = getNextPageNumberFromPage(singlePage);
    }

    // Free space from single page
    free(singlePage);

    // Write to inout maxSize
    if (maxSize!=NULL){
        *maxSize = pageCount*pageDataSize;
    }

    return record;
}

//////////////////////////////////////////////////////////////////
//  WRITE

/**
 * Programs data into a page chain in storage.
 *
 * @param      data                      Data to be written to storage.
 * @param      size                      Size of data to be written (in bytes).
 * @param      overwriteChainBeginning   Optional: Page number of the beginning of an existing chain to overwrite with data
 * @param      isOverwriteChainSingle    Optional: Boolean to indicate if overwrite chain is single paged
 * @param[out] wasSinglePage             Indicates if data was saved into a single page.
 * @param[out] remains                   Optional: Indicates how much space was left empty in the last page written.
 *
 * @return Number of first page in the chain. Returns 0 (illegal page) to indicate insufficient space to store or invalid call.
 */
static PageNumber writeData(PageDataRef data, KVATSize size, PageNumber overwriteChainBeginning, bool isOverwriteChainSingle, bool* wasSinglePage, KVATSize* remains){
    if (size==0){return 0;}

    // Calculate if data fits in single page
    bool isSinglePage = size>index->pageSize ? false : true;

    // Calculate the page segment sizes
    KVATSize pageSize = index->pageSize;
    KVATSize pageNextSize = getPageNextSize(isSinglePage);
    KVATSize pageDataSize = pageSize-pageNextSize;

    // Calculate pages needed (easy when it's single page)
    PageNumber pagesNeeded = isSinglePage ? 1 : size/pageDataSize;
    if (!isSinglePage && size%pageDataSize){ // Get crude ceil of division
        pagesNeeded++;
    }

    // Get buffer to hold page data when assembling before saving
    PageDataRef pageData = malloc(index->pageSize);

    // Prepare places to keep track of the pages
    PageNumber* pagesUsed = malloc(sizeof(PageNumber)*pagesNeeded);
    PageNumber thisPageN = 0;
    PageNumber nextPageN = overwriteChainBeginning ? overwriteChainBeginning : getEmptyPageNumber(true);
    PageNumber overwriteChainNext = overwriteChainBeginning ? overwriteChainBeginning : 0;// Next from last page used from overwritten chain, if any

    for (PageNumber currentPageI = 0; currentPageI<pagesNeeded; currentPageI++){
        // Try to cycle to the next overwriteChainNext
        if (overwriteChainNext && !isOverwriteChainSingle){ // If the "next" from last loop was from a multiple page chain, maybe there is more.
            overwriteChainNext = readNextPageNumber(overwriteChainNext);

        }else if(overwriteChainNext!=0){
            overwriteChainNext = 0;
        }

        // Cycle to the next page and check it
        thisPageN = nextPageN;

        if (thisPageN==0){// Looks like no more pages were available, return all pages used and fail gracefully.
            for (PageNumber returnI = 0; returnI<currentPageI; returnI++){
                markPageInRecord(pagesUsed[returnI], false);    // Mark as not used
            }
            // Invalidate first page so caller knows that write operation failed
            pagesUsed[0] = 0;
            break;

        }else{
            // Mark in local tracker
            pagesUsed[currentPageI] = thisPageN;
        }

        // See if we will need a next page in the next loop, and get it ready
        if (currentPageI+1<pagesNeeded){ // Need another page

            // Try to reuse chain if available
            nextPageN = overwriteChainNext ? overwriteChainNext : getEmptyPageNumber(true);

        }else{ // No more pages needed

            nextPageN = 0;

        }

        // Write next page number into the working page
        memcpy(pageData, &nextPageN, pageNextSize);

        // Write actual data - cast to char* [legal move] to do pointer arithmetic
        memcpy((char*)pageData+pageNextSize, (char*)data+pageDataSize*currentPageI, pageDataSize);

        // Page is complete, now put it on storage
        writePage(pageData, thisPageN);

    }

    // free allocated buffer
    free(pageData);

    // Free pages used tracker
    free(pagesUsed);

    // Write to inout wasSinglePage
    *wasSinglePage = isSinglePage;

    // Write to inout remains
    if (remains!=NULL){
        *remains = pageDataSize - (size%pageDataSize);
    }

    // Take care of overwrite chain if not all was used
    if (overwriteChainNext){
        followPageChainAndSetPageRecord(overwriteChainNext, false, isOverwriteChainSingle);
    }

    // Return page number of first page
    return pagesUsed[0];

}

//////////////////////////////////////////////////////////////////
//  LOOKUP

static PageNumber lookupByKey(char* key, bool isPartialKey, PageNumber entryNumberSearchStart){
    PageNumber match = 0;   // To keep the entry that matched

    // Prepare preallocated buffer for multiple key fetches, keep a separate definition for actual return
    char entryKeyPreallocBuff[STRINGKEYSTDLEN];
    char* entryKey;

    // Get the length of the key being searched
    KVATSize keySize = strlen(key);

    PageNumber entryCount = index->pageCount;   // Entry count equals page count
    KVATKeyValueEntry entry;                    // Used to store current entry
    int keyCompare;
    KVATSize entryKeySize;
    for (PageNumber entryN = entryNumberSearchStart ? entryNumberSearchStart : 1; entryN<entryCount; entryN++){

        // Read entry from storage
        readTableEntry(&entry, entryN);

        // Check if entry is active
        if (entry.metadata & MACTIVE){

            // Fetch the key
            entryKey = (char*)fetchData(entry.keyPage, entry.metadata & MKEYCHAIN, NULL, (PageDataRef)entryKeyPreallocBuff, STRINGKEYSTDLEN);

            // Check key
            entryKeySize = strlen(entryKey);

            // Compare em
            keyCompare = strncmp(key, entryKey, keySize);

            // Cleanup. Yes, at this point is fine. Shall not forget to free this possible allocated space.
            if (entryKey != entryKeyPreallocBuff){ // In the case that the fetch had to reserve a longer buffer
                free(entryKey);
            }

            // Size check
            if ( (isPartialKey && keySize<=entryKeySize) || (!isPartialKey && keySize==entryKeySize)){

                if (!keyCompare){ // 0  means equal
                    match = entryN;
                    break;
                }
            }


        }
    }

    return match;
}

//////////////////////////////////////////////////////////////////
//  PUBLIC SAVE

/**
 * Saves a string of data tagged with a key
 *
 * @param      key            String tag for the value to save
 * @param      value          Reference to value to save in storage
 * @param      valueSize      Length of the value to save
 *
 * @return
 */
KVATException KVATSaveValue(char* key, void* value, KVATSize valueSize){
    // Get empty table entry for new, or existing for overwrite
    PageNumber tableEntryN = lookupByKey(key, false, 1);   // Look for same string (overwrite)
    bool isOverwrite = true;
    if (tableEntryN==0){
        tableEntryN = getEmptyTableEntryNumber();   // Get new entry
        isOverwrite = false;
    }
    // Guard
    if (tableEntryN==0){return KVATException_insufficientSpace;}

    // Get local table entry. No need to read the entry's current value from storage if not overwriting -it's empty-
    KVATKeyValueEntry tableEntry = {};
    if (isOverwrite){
        readTableEntry(&tableEntry, tableEntryN);
    }

    // Set is as open and save with that status
    tableEntry.metadata |= MOPEN;   // All that matters is that it's open, but keep old stuff in case of overwrite
    saveTableEntry(&tableEntry, tableEntryN);

    bool keySavedInSinglePage, valueSavedInSinglePage;
    KVATSize valueRemains;

    // Try to save the key if not overwrite
    if (!isOverwrite){
        PageNumber keyStartPage = writeData((PageDataRef)key, strlen(key)+1, NULL, NULL, &keySavedInSinglePage, NULL);
        // Guard
        if (keyStartPage==0){return KVATException_insufficientSpace;}
        // Save start page
        tableEntry.keyPage = keyStartPage;
    }

    // Prepare overwrite variables (if needed)
    PageNumber overwriteChainStart = isOverwrite ? tableEntry.valuePage : NULL; // The start page of the old chain
    bool isOverwriteChainSingle = tableEntry.metadata & MVALUECHAIN;

    // Try to save the data (value)
    PageNumber valueStartPage = writeData((PageDataRef)value, valueSize, overwriteChainStart, isOverwriteChainSingle, &valueSavedInSinglePage, &valueRemains);
    // Guard
    if (valueStartPage==0){return KVATException_insufficientSpace;}
    // Save start page
    tableEntry.valuePage = valueStartPage;

    // Set right metadata.
    if (isOverwrite){
        tableEntry.metadata &= MKEYCHAIN;   // Only keep previous key settings
    }else{
        tableEntry.metadata = keySavedInSinglePage ? MKC_SINGLE : MKC_MULTIPLE; // Reset previous contents with new key settings
    }
    tableEntry.metadata |= MACTIVE | (valueSavedInSinglePage ? MVC_SINGLE : MVC_MULTIPLE) | MKF_STRING;

    // Save remains
    tableEntry.remains = valueRemains;

    // Save entry to storage
    saveTableEntry(&tableEntry, tableEntryN);

    return KVATException_none;
}

//////////////////////////////////////////////////////////////////
//  PUBLIC RETRIEVE

/**
 * Returns pointer to value corresponding to a key
 *
 * @param      key            String tag for the value to retrieve
 * @param[out] size           Size of the value returned in bytes.
 *
 * @return Pointer to allocated space including the retrieved value. NULL if no match found.
 */
void* KVATRetrieveValue(char* key, KVATSize* size){
    // Assert
    if (!key){return NULL;}

    // Look for this thing
    PageNumber tableEntryN = lookupByKey(key, false, 1);   // Look for same string (overwrite)

    if (tableEntryN==0){return NULL;}

    // Get Record
    KVATKeyValueEntry tableEntry;
    readTableEntry(&tableEntry, tableEntryN);

    KVATSize maxSize = 0;

    // Read value
    PageDataRef value = fetchData(tableEntry.valuePage, tableEntry.metadata & MVALUECHAIN, &maxSize, NULL, NULL);

    // Calculate actual size
    if (size!=NULL){
        *size = maxSize-tableEntry.remains;
    }

    return value;
}

//////////////////////////////////////////////////////////////////

bool KVATInit(){
    // Enable the EEPROM module.
    SysCtlPeripheralEnable(SYSCTL_PERIPH_EEPROM0);

    // Wait for the EEPROM module to be ready.
    while(!SysCtlPeripheralReady(SYSCTL_PERIPH_EEPROM0));

    uint32_t memInitStatus = EEPROMInit();
    if (memInitStatus==EEPROM_INIT_ERROR){

        return false;
    }

    //Get space for the index
    index = malloc(sizeof(KVATIndex));

    if (index==NULL){return false;}

    // Read current index from system
    readIndex();

    //Check format ID
    if (index->formatID!=FORMATID){// Need to format memory
        formatMemory();
    }

    // Get page record
    updatePageRecord();

    isInit = true;
    return true;
}