#include <type_traits>
#include <sys/types.h>
#include <dirent.h>

#include <seqan/arg_parse.h>
#include <seqan/seq_io.h>
#include <seqan/index.h>

#include "../include/lambda/src/mkindex_saca.hpp"
#include "../include/lambda/src/mkindex_misc.hpp"
#include "../include/lambda/src/mkindex_algo.hpp"

#include "common.hpp"

using namespace seqan;

struct IndexOptions
{
    CharString indexPath;
    uint64_t seqNumber;
    uint64_t maxSeqLength;
    uint64_t totalLength;
    unsigned sampling;
    bool directory;
    bool useRadix;
    bool verbose;
};

inline std::string extractFileName(std::string const & path)
{
    // possible formats of 'path': ./file.fa   file.fa   ../file.fa   /path/to/file.fa
    auto const pos = path.find_last_of('/');
    if (pos == std::string::npos) // no slash found, i.e. file.fa
        return path;
    else
        return path.substr(pos + 1);
}

template <typename TSeqNo, typename TSeqPos, typename TBWTLen,
          typename TString, typename TStringSetConfig, typename TRadixSortTag>
void buildIndex(StringSet<TString, TStringSetConfig> /*const*/ & chromosomes, IndexOptions const & options,
                       TRadixSortTag const & /**/)
{
    using TText = StringSet<TString, Owner<ConcatDirect<SizeSpec_<TSeqNo, TSeqPos> > > > ;
    using TFMIndexConfig = TGemMapFastFMIndexConfig<TBWTLen>;
    TFMIndexConfig::SAMPLING = options.sampling;
    using TUniIndexConfig = FMIndex<TRadixSortTag, TFMIndexConfig>;

    TText chromosomesConcat(chromosomes);
    clear(chromosomes);

    {
        uint32_t const bwtDigits = std::numeric_limits<TBWTLen>::digits;
        uint32_t const seqNoDigits = std::numeric_limits<TSeqNo>::digits;
        uint32_t const seqPosDigits = std::numeric_limits<TSeqPos>::digits;

        // Print some size information on the index.
        if (options.verbose)
        {
            CharString const alphabet = std::is_same<typename Value<TString>::Type, Dna>::value ? "dna4" : "dna5";
            std::cout << "Index will be constructed using " << alphabet << " alphabet.\n"
                         "- The BWT is represented by " << bwtDigits << " bit values.\n"
                         "- The sampled suffix array is represented by pairs of " << seqNoDigits <<
                         " and " << seqPosDigits << " bit values." << std::endl;
        }

        // Store index dimensions and alphabet type.
        StringSet<CharString, Owner<ConcatDirect<> > > info;
        uint32_t const alphabetSize = 4 + std::is_same<typename Value<TString>::Type, Dna5>::value;
        std::string const directoryFlag = options.directory ? "true" : "false";
        appendValue(info, "alphabet_size:" + std::to_string(alphabetSize));
        appendValue(info, "sa_dimensions_i1:" + std::to_string(seqNoDigits));
        appendValue(info, "sa_dimensions_i2:" + std::to_string(seqPosDigits));
        appendValue(info, "bwt_dimensions:" + std::to_string(bwtDigits));
        appendValue(info, "sampling_rate:" + std::to_string(options.sampling));
        appendValue(info, "fasta_directory:" + directoryFlag);
        save(info, toCString(std::string(toCString(options.indexPath)) + ".info"));
    }

    // TODO: do not use radix for small files
    // TODO: print helpful information if running out of memory.

    {
        Index<TText, TUniIndexConfig> fwdIndex(chromosomesConcat);
        SEQAN_IF_CONSTEXPR (std::is_same<TRadixSortTag, RadixSortSACreateTag>::value)
            indexCreateProgress(fwdIndex, FibreSALF());
        else
        {
            std::cout << "Create fwd Index ... " << std::flush;
            indexCreate(fwdIndex, FibreSALF());
            std::cout << "done!\n";
        }
        save(fwdIndex, toCString(options.indexPath));
    }

    {
        reverse(chromosomesConcat);
        Index<TText, TUniIndexConfig> bwdIndex(chromosomesConcat);
        SEQAN_IF_CONSTEXPR (std::is_same<TRadixSortTag, RadixSortSACreateTag>::value)
            indexCreateProgress(bwdIndex, FibreSALF());
        else
        {
            std::cout << "Create bwd Index ... " << std::flush;
            indexCreate(bwdIndex, FibreSALF());
            std::cout << "done!\n";
        }
        clear(getFibre(getFibre(getFibre(bwdIndex, FibreSA()), FibreSparseString()), FibreValues()));
        clear(getFibre(getFibre(getFibre(bwdIndex, FibreSA()), FibreSparseString()), FibreIndicators()));
        saveRev(bwdIndex, toCString(std::string(toCString(options.indexPath)) + ".rev"));
    }
}

template <typename TString, typename TStringSetConfig, typename TRadixSortTag>
void buildIndex(StringSet<TString, TStringSetConfig> /*const*/ & chromosomes, IndexOptions const & options,
                       TRadixSortTag const & /**/)
{
    constexpr uint64_t max16bitUnsignedValue = std::numeric_limits<uint16_t>::max();
    constexpr uint64_t max32bitUnsignedValue = std::numeric_limits<uint32_t>::max();

    // Analyze dimensions of the index needed.
    // NOTE: actually <= maxXXbitUnsignedValue+1 should be sufficient
    if (options.seqNumber <= max16bitUnsignedValue && options.maxSeqLength <= max32bitUnsignedValue)
    {
        if (options.totalLength <= max32bitUnsignedValue)
            buildIndex<uint16_t, uint32_t, uint32_t>(chromosomes, options, TRadixSortTag()); // e.g. human genome
        else
            buildIndex<uint16_t, uint32_t, uint64_t>(chromosomes, options, TRadixSortTag()); // e.g. barley genome
    }
    else if (options.seqNumber <= max32bitUnsignedValue && options.maxSeqLength <= max16bitUnsignedValue)
        buildIndex<uint32_t, uint16_t, uint64_t>(chromosomes, options, TRadixSortTag()); // e.g. read data set
    else
        buildIndex<uint64_t, uint64_t, uint64_t>(chromosomes, options, TRadixSortTag()); // anything else
}

template <typename TString, typename TStringSetConfig>
int buildIndex(StringSet<TString, TStringSetConfig> /*const*/ & chromosomes, IndexOptions const & options)
{
#ifdef NDEBUG
    try
    {
        if (options.useRadix)
            buildIndex(chromosomes, options, RadixSortSACreateTag());
        else
            buildIndex(chromosomes, options, Nothing());
    }
    catch (std::bad_alloc const & e)
    {
        std::cerr << "ERROR: GenMap ran out of memory :(\n"
                     "       You might want to use a different algorithm (-A skew or -A radix).\n";
        return -1;
    }
    catch (std::exception const & e)
    {
        std::cerr << "\n\n"
                  << "ERROR: The following unspecified exception was thrown:\n"
                  << "       \"" << e.what() << "\"\n"
                  << "       If the problem persists, report an issue at "
                  << "https://github.com/cpockrandt/genmap/issues "
                  << "and include this output, as well as the output of `genmap --version`, thanks!\n";
        return -1;
    }
#else
    // In debug mode we don't catch the exceptions so that we get a backtrace from SeqAn's handler
    if (options.useRadix)
        buildIndex(chromosomes, options, RadixSortSACreateTag());
    else
        buildIndex(chromosomes, options, Nothing());
#endif

    std::cout << "Index created successfully.\n";

    return 0;
}

int indexMain(int const argc, char const ** argv)
{
    // Argument Parser
    ArgumentParser parser("GenMap index");
    sharedSetup(parser);
    addDescription(parser, "Index creation. Only supports Dna (A, C, G, T).");

    // sorted in descending lexicographical order, since setValidValues() prints them in this order
    std::vector<std::string> const fastaFileTypes {"fsa", "fna", "fastq", "fasta", "fas", "fa"};
    std::string fastaFileTypesHelpString;
    for (uint8_t i = 0; i < fastaFileTypes.size() - 1; ++i)
        fastaFileTypesHelpString += '.' + fastaFileTypes[i] + ' ';
    fastaFileTypesHelpString += "and ." + fastaFileTypes.back();

    addOption(parser, ArgParseOption("F", "fasta-file", "Path to the fasta file.", ArgParseArgument::INPUT_FILE, "IN"));
    setValidValues(parser, "fasta-file", fastaFileTypes);

    addOption(parser, ArgParseOption("FD", "fasta-directory", "Path to the directory of fasta files (indexes all " + fastaFileTypesHelpString + " files in there, not including subdirectories).", ArgParseArgument::INPUT_FILE, "IN"));

    addOption(parser, ArgParseOption("I", "index", "Path to the index.", ArgParseArgument::OUTPUT_FILE, "OUT"));
    setRequired(parser, "index");

    // TODO: describe both algorithms in terms of space consumption (disk and RAM)
    addOption(parser, ArgParseOption("A", "algorithm", "Algorithm for suffix array construction (needed for the FM index).", ArgParseArgument::INPUT_FILE, "IN"));
    setDefaultValue(parser, "algorithm", "radix");
    setValidValues(parser, "algorithm", std::vector<std::string>{"radix", "skew"});

    addOption(parser, ArgParseOption("S", "sampling", "Sampling rate of suffix array", ArgParseArgument::INTEGER, "INT"));
    setDefaultValue(parser, "sampling", 10);
    setMaxValue(parser, "sampling", "64");
    setMinValue(parser, "sampling", "1");

    addOption(parser, ArgParseOption("v", "verbose", "Outputs some additional information on the constructed index."));

    addOption(parser, ArgParseOption("xa", "seqno", "Number of sequences.", ArgParseArgument::INTEGER, "INT"));
    hideOption(parser, "seqno");

    addOption(parser, ArgParseOption("xb", "seqpos", "Max length of sequences.", ArgParseArgument::INTEGER, "INT"));
    hideOption(parser, "seqpos");

    addOption(parser, ArgParseOption("xc", "bwtlen", "Total length of all sequences.", ArgParseArgument::INTEGER, "INT"));
    hideOption(parser, "bwtlen");

    // TODO: hint, that indexing may take some time, i.e., 2-3 hours for the human genome with Skew
    // TODO: if non Dna-character found (IUPAC): suggest converting them to Dna5.

    ArgumentParser::ParseResult res = parse(parser, argc, argv);
    if (res != ArgumentParser::PARSE_OK)
        return res == ArgumentParser::PARSE_ERROR;

    bool const isSetFastaFile = isSet(parser, "fasta-file");
    bool const isSetFastaDirectory = isSet(parser, "fasta-directory");

    if (isSetFastaFile && isSetFastaDirectory)
    {
        std::cerr << "ERROR: You can only use eiher --fasta-file or --fasta-directory, not both.\n";
        return ArgumentParser::PARSE_ERROR;
    }
    else if (!isSetFastaFile && !isSetFastaDirectory)
    {
        std::cerr << "ERROR: You forgot to specify --fasta-file or --fasta-directory.\n";
        return ArgumentParser::PARSE_ERROR;
    }

    // Retrieve input parameter
    IndexOptions options;
    CharString fastaPath, algorithm;
    getOptionValue(options.indexPath, parser, "index");
    getOptionValue(algorithm, parser, "algorithm");
    getOptionValue(options.sampling, parser, "sampling");
    toLower(algorithm);
    options.directory = isSetFastaDirectory;
    if (isSetFastaDirectory)
        getOptionValue(fastaPath, parser, "fasta-directory");
    else
        getOptionValue(fastaPath, parser, "fasta-file");
    options.useRadix = algorithm == "radix";
    options.verbose = isSet(parser, "verbose");

    // Check whether the output path exists and is writeable!
    if (fileExists(toCString(options.indexPath)))
    {
        std::cerr << "ERROR: The output directory for the index already exists at " << options.indexPath << '\n'
             << "Please remove it, or choose a different location.\n";
        return ArgumentParser::PARSE_ERROR;
    }
    else if (mkdir(toCString(options.indexPath), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH))
    {
        std::cerr << "ERROR: Cannot create output directory at " << options.indexPath << '\n';
        return ArgumentParser::PARSE_ERROR;
    }

    // Append prefix name for indices.
    if (back(options.indexPath) != '/')
        options.indexPath += '/';
    options.indexPath += "index";

    // Read fasta input file(s)
    StringSet<Dna5String> chromosomes;
    StringSet<CharString, Owner<ConcatDirect<> > > directoryInformation;

    if (options.directory)
    {
        std::vector<std::string> filenames;
        DIR * dirp = opendir(toCString(fastaPath));
        struct dirent * dp;
        while ((dp = readdir(dirp)) != NULL)
        {
            std::string const file(dp->d_name);
            std::string const fileExtension = file.substr(file.find_last_of('.') + 1);
            if (std::find(fastaFileTypes.begin(), fastaFileTypes.end(), fileExtension) != fastaFileTypes.end())
                filenames.push_back(file);
        }
        closedir(dirp);
        std::sort(filenames.begin(), filenames.end());

        for (auto const & file : filenames)
        {
            std::string fullPath = toCString(fastaPath);
            if (fullPath.back() != '/')
                fullPath += '/';
            fullPath += file;
            SeqFileIn seqFileIn(toCString(fullPath));

            StringSet<CharString, Owner<ConcatDirect<> > > ids;
            StringSet<Dna5String> chromosomes2;
            readRecords(ids, chromosomes2, seqFileIn);
            if (lengthSum(chromosomes2) == 0)
            {
                std::cerr << "WARNING: The fasta file " << file << " seems to be empty. Excluded from indexing.\n";
                continue;
            }

            for (uint64_t i = 0; i < length(chromosomes2); ++i)
            {
                // skip empty sequences
                if (length(chromosomes2[i]) == 0)
                    continue;

                std::string const id = toCString(static_cast<CharString>(ids[i]));
                std::string const len = std::to_string(length(chromosomes2[i]));
                appendValue(directoryInformation, file + ";" + len + ";" + id); // toCString(id.substr(0, id.find(" ")))
                appendValue(chromosomes, chromosomes2[i]);
            }

            clear(ids);
        }

        if (length(chromosomes) == 0)
        {
            // TODO: rmdir
            std::cerr << "ERROR: No non-empty fasta file found!\n";
            return ArgumentParser::PARSE_ERROR;
        }

        std::cout << filenames.size() << " fasta files have been loaded";
        if (options.verbose)
        {
            std::cout << ":\n";
            std::copy(filenames.begin(), filenames.end(), std::ostream_iterator<std::string>(std::cout, "\n"));
        }
        else
        {
            std::cout << " (run with --verbose to list the files)\n";
        }
    }
    else
    {
        StringSet<CharString, Owner<ConcatDirect<> > > ids;
        StringSet<Dna5String> chromosomes2;

        SeqFileIn seqFileIn(toCString(fastaPath));
        readRecords(ids, chromosomes2, seqFileIn);
        if (options.verbose)
            std::cout << "Number of sequences in the fasta file: " << length(chromosomes) << '\n';

        for (uint64_t i = 0; i < length(chromosomes2); ++i)
        {
            // skip empty sequences
            if (length(chromosomes2[i]) == 0)
                continue;

            std::string const id = toCString(static_cast<CharString>(ids[i]));
            std::string const len = std::to_string(length(chromosomes2[i]));
            std::string const file = extractFileName(toCString(fastaPath));
            appendValue(directoryInformation, file + ";" + len + ";" + id); // toCString(id.substr(0, id.find(" ")))
            appendValue(chromosomes, chromosomes2[i]);
        }

        if (length(chromosomes) == 0)
        {
            // TODO: rmdir
            std::cerr << "ERROR: The fasta file seems to be empty.\n";
            return ArgumentParser::PARSE_ERROR;
        }
    }

    save(directoryInformation, toCString(std::string(toCString(options.indexPath)) + ".ids"));

    // check whether it can be converted to Dna4 and analyze the data for determining the index dimensions later.
    bool canConvert = true; // TODO: test this code block
    options.seqNumber = length(chromosomes);
    options.maxSeqLength = 0;
    options.totalLength = length(chromosomes); // to account for a sentinel character for each chromosome in the FM index.
    for (uint64_t i = 0; i < length(chromosomes); ++i)
    {
        options.totalLength += length(chromosomes[i]);
        options.maxSeqLength = std::max<uint64_t>(options.maxSeqLength, length(chromosomes[i]));

        for (uint64_t j = 0; canConvert && j < length(chromosomes[i]); ++j)
        {
            if (chromosomes[i][j] == 'N')
                canConvert = false;
        }
    }

    // overwrite dimensions
    if (isSet(parser, "seqno"))
    {
        uint64_t seqno;
        getOptionValue(seqno, parser, "seqno");
        options.seqNumber = (static_cast<uint64_t>(1) << seqno) - 2;
    }
    if (isSet(parser, "seqpos"))
    {
        uint64_t seqpos;
        getOptionValue(seqpos, parser, "seqpos");
        options.maxSeqLength = (static_cast<uint64_t>(1) << seqpos) - 2;
    }
    if (isSet(parser, "bwtlen"))
    {
        uint64_t bwtlen;
        getOptionValue(bwtlen, parser, "bwtlen");
        options.totalLength = (static_cast<uint64_t>(1) << bwtlen) - 2;
    }

    if (options.useRadix && lengthSum(chromosomes) < 1'000'000)
    {
        options.useRadix = false;
        std::cout << "NOTE: Your input is quite small (i.e., less than 1 mega base)."
                  << "Hence, Skew7 is used for index construction anyway to avoid parallelization overhead.\n"
                  << std::flush;
    }

    // Construct index using Dna4 or Dna5 alphabet.
    if (canConvert)
    {
        // TODO: avoid copying. Use a custom ModfiedFunctor instead.
        StringSet<DnaString> chromosomes4(chromosomes);
        clear(chromosomes);
        return buildIndex(chromosomes4, options);
    }
    else
    {
        return buildIndex(chromosomes, options);
    }
}
