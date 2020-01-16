/* vim: set sw=4 sts=4 et foldmethod=syntax : */

#include "formats/bigraph.hh"
#include "homomorphism.hh"
#include "lackey.hh"
#include "symmetries.hh"
#include "proof.hh"
#include "restarts.hh"

#include <boost/program_options.hpp>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <vector>

#include <unistd.h>

namespace po = boost::program_options;

using std::boolalpha;
using std::cerr;
using std::cout;
using std::endl;
using std::exception;
using std::function;
using std::ifstream;
using std::localtime;
using std::make_pair;
using std::make_shared;
using std::make_unique;
using std::put_time;
using std::string;
using std::vector;

using std::chrono::duration_cast;
using std::chrono::milliseconds;
using std::chrono::operator""s;
using std::chrono::seconds;
using std::chrono::steady_clock;
using std::chrono::system_clock;

auto main(int argc, char * argv[]) -> int
{
    try {
        po::options_description display_options{ "Program options" };
        display_options.add_options()
            ("help",                                         "Display help information")
            ("timeout",            po::value<int>(),         "Abort after this many seconds")
            ("parallel",                                     "Use auto-configured parallel search (highly nondeterministic runtimes)");

        po::options_description problem_options{ "Problem options" };
        problem_options.add_options()
            ("count-solutions",                              "Count the number of solutions")
            ("print-all-solutions",                          "Print out every solution, rather than one");
        display_options.add(problem_options);

        po::options_description search_options{ "Advanced search configuration options" };
        search_options.add_options()
            ("restarts",             po::value<string>(),      "Specify restart policy (luby / geometric / timed / none)")
            ("geometric-multiplier", po::value<double>(),      "Specify multiplier for geometric restarts")
            ("geometric-constant",   po::value<double>(),      "Specify starting constant for geometric restarts")
            ("restart-interval",     po::value<int>(),         "Specify the restart interval in milliseconds for timed restarts")
            ("restart-minimum",      po::value<int>(),         "Specify a minimum number of backtracks before a timed restart can trigger")
            ("luby-constant",        po::value<int>(),         "Specify the starting constant / multiplier for Luby restarts")
            ("value-ordering",       po::value<string>(),      "Specify value-ordering heuristic (biased / degree / antidegree / random)")
            ("pattern-symmetries",                             "Eliminate pattern symmetries (requires Gap)");
        display_options.add(search_options);

        po::options_description mangling_options{ "Advanced input processing options" };
        mangling_options.add_options()
            ("no-clique-detection",                            "Disable clique / independent set detection")
            ("no-isolated-vertex-removal",                     "Disable isolated vertex removal")
            ("no-supplementals",                               "Do not use supplemental graphs")
            ("no-nds",                                         "Do not use neighbourhood degree sequences");
        display_options.add(mangling_options);

        po::options_description parallel_options{ "Advanced parallelism options" };
        parallel_options.add_options()
            ("threads",              po::value<unsigned>(),    "Use threaded search, with this many threads (0 to auto-detect)")
            ("triggered-restarts",                             "Have one thread trigger restarts (more nondeterminism, better performance)")
            ("delay-thread-creation",                          "Do not create threads until after the first restart");
        display_options.add(parallel_options);

        vector<string> pattern_less_thans;
        po::options_description symmetry_options{ "Manual symmetry options" };
        symmetry_options.add_options()
            ("pattern-less-than",   po::value<vector<string> >(&pattern_less_thans),
                                                               "Specify a pattern less than constraint, in the form v<w")
            ("pattern-automorphism-group-size", po::value<string>(),
                                                               "Specify the size of the pattern graph automorphism group");
        display_options.add(symmetry_options);

        po::options_description lackey_options{ "External constraint solver options" };
        lackey_options.add_options()
            ("send-to-lackey",      po::value<string>(),       "Send candidate solutions to an external solver over this named pipe")
            ("receive-from-lackey", po::value<string>(),       "Receive responses from external solver over this named pipe");
        display_options.add(lackey_options);

        po::options_description all_options{ "All options" };
        all_options.add_options()
            ("pattern-file", "specify the pattern file")
            ("target-file",  "specify the target file")
            ;

        all_options.add(display_options);

        po::positional_options_description positional_options;
        positional_options
            .add("pattern-file", 1)
            .add("target-file", 1)
            ;

        po::variables_map options_vars;
        po::store(po::command_line_parser(argc, argv)
                .options(all_options)
                .positional(positional_options)
                .run(), options_vars);
        po::notify(options_vars);

        /* --help? Show a message, and exit. */
        if (options_vars.count("help")) {
            cout << "Usage: " << argv[0] << " [options] pattern target" << endl;
            cout << endl;
            cout << display_options << endl;
            return EXIT_SUCCESS;
        }

        /* No algorithm or no input file specified? Show a message and exit. */
        if (! options_vars.count("pattern-file") || ! options_vars.count("target-file")) {
            cout << "Usage: " << argv[0] << " [options] pattern target" << endl;
            return EXIT_FAILURE;
        }

        /* Figure out what our options should be. */
        HomomorphismParams params;

        params.injectivity = Injectivity::Injective;
        params.induced = true;
        params.bigraph = true;
        params.count_solutions = options_vars.count("count-solutions") || options_vars.count("print-all-solutions");
        params.minimal_unsat_pattern = false;

        params.triggered_restarts = options_vars.count("triggered-restarts") || options_vars.count("parallel");

        if (options_vars.count("threads"))
            params.n_threads = options_vars["threads"].as<unsigned>();
        else if (options_vars.count("parallel"))
            params.n_threads = 0;

        if (options_vars.count("delay-thread-creation") || options_vars.count("parallel"))
            params.delay_thread_creation = true;

        if (options_vars.count("restarts")) {
            string restarts_policy = options_vars["restarts"].as<string>();
            if (restarts_policy == "luby") {
                unsigned long long multiplier = LubyRestartsSchedule::default_multiplier;
                if (options_vars.count("luby-constant"))
                    multiplier = options_vars["luby-constant"].as<int>();
                params.restarts_schedule = make_unique<LubyRestartsSchedule>(multiplier);
            }
            else if (restarts_policy == "geometric") {
                double geometric_constant = GeometricRestartsSchedule::default_initial_value;
                double geometric_multiplier = GeometricRestartsSchedule::default_multiplier;
                if (options_vars.count("geometric-constant"))
                    geometric_constant = options_vars["geometric-constant"].as<double>();
                if (options_vars.count("geometric-multiplier"))
                    geometric_multiplier = options_vars["geometric-multiplier"].as<double>();
                params.restarts_schedule = make_unique<GeometricRestartsSchedule>(geometric_constant, geometric_multiplier);
            }
            else if (restarts_policy == "timed") {
                milliseconds duration = TimedRestartsSchedule::default_duration;
                unsigned long long minimum_backtracks = TimedRestartsSchedule::default_minimum_backtracks;
                if (options_vars.count("restart-interval"))
                    duration = milliseconds{ options_vars["restart-interval"].as<int>() };
                if (options_vars.count("restart-minimum"))
                    minimum_backtracks = options_vars["restart-minimum"].as<int>();
                params.restarts_schedule = make_unique<TimedRestartsSchedule>(duration, minimum_backtracks);
            }
            else if (restarts_policy == "none") {
                params.restarts_schedule = make_unique<NoRestartsSchedule>();
            }
            else {
                cerr << "Unknown restarts policy '" << restarts_policy << "'" << endl;
                return EXIT_FAILURE;
            }
        }
        else {
            if (params.count_solutions)
                params.restarts_schedule = make_unique<NoRestartsSchedule>();
            else if (options_vars.count("parallel"))
                params.restarts_schedule = make_unique<TimedRestartsSchedule>(TimedRestartsSchedule::default_duration, TimedRestartsSchedule::default_minimum_backtracks);
            else
                params.restarts_schedule = make_unique<LubyRestartsSchedule>(LubyRestartsSchedule::default_multiplier);
        }

        if (options_vars.count("value-ordering")) {
            string value_ordering_heuristic = options_vars["value-ordering"].as<string>();
            if (value_ordering_heuristic == "biased")
                params.value_ordering_heuristic = ValueOrdering::Biased;
            else if (value_ordering_heuristic == "degree")
                params.value_ordering_heuristic = ValueOrdering::Degree;
            else if (value_ordering_heuristic == "antidegree")
                params.value_ordering_heuristic = ValueOrdering::AntiDegree;
            else if (value_ordering_heuristic == "random")
                params.value_ordering_heuristic = ValueOrdering::Random;
            else {
                cerr << "Unknown value-ordering heuristic '" << value_ordering_heuristic << "'" << endl;
                return EXIT_FAILURE;
            }
        }

        string pattern_automorphism_group_size = "1";
        bool was_given_automorphism_group = false;
        if (options_vars.count("pattern-automorphism-group-size")) {
            pattern_automorphism_group_size = options_vars["pattern-automorphism-group-size"].as<string>();
            was_given_automorphism_group = true;
        }

        for (auto & s : pattern_less_thans) {
            auto p = s.find('<');
            if (p == string::npos) {
                cerr << "Invalid pattern less-than constraint '" << s << "'" << endl;
                return EXIT_FAILURE;
            }
            auto a = s.substr(0, p), b = s.substr(p + 1);
            params.pattern_less_constraints.emplace_back(a, b);
        }

        if (options_vars.count("send-to-lackey") ^ options_vars.count("receive-from-lackey")) {
            cerr << "Must specify both of --send-to-lackey and --receive-from-lackey" << endl;
            return EXIT_FAILURE;
        }

        char hostname_buf[255];
        if (0 == gethostname(hostname_buf, 255))
            cout << "hostname = " << string(hostname_buf) << endl;
        cout << "commandline =";
        for (int i = 0 ; i < argc ; ++i)
            cout << " " << argv[i];
        cout << endl;

        auto started_at = system_clock::to_time_t(system_clock::now());
        cout << "started_at = " << put_time(localtime(&started_at), "%F %T") << endl;

        /* Read in the graphs */

        string pattern_filename = options_vars["pattern-file"].as<string>();
        string target_filename = options_vars["target-file"].as<string>();

        ifstream pattern_infile{ pattern_filename };
        if (! pattern_infile)
            throw GraphFileError{ pattern_filename, "unable to open pattern file", false };

        ifstream target_infile{ target_filename };
        if (! target_infile)
            throw GraphFileError{ target_filename, "unable to open target file", false };

        auto graphs = make_pair(
            read_pattern_bigraph(move(pattern_infile), pattern_filename),
            read_target_bigraph(move(target_infile), target_filename));

        cout << "pattern_file = " << pattern_filename << endl;
        cout << "target_file = " << target_filename << endl;

        if (options_vars.count("send-to-lackey") && options_vars.count("receive-from-lackey")) {
            params.lackey = make_unique<Lackey>(
                    options_vars["send-to-lackey"].as<string>(),
                    options_vars["receive-from-lackey"].as<string>(),
                    graphs.first,
                    graphs.second);
        }

        if (options_vars.count("print-all-solutions")) {
            params.enumerate_callback = [&] (const VertexToVertexMapping & mapping) {
                cout << "mapping = ";
                for (auto v : mapping)
                    cout << "(" << graphs.first.vertex_name(v.first) << " -> " << graphs.second.vertex_name(v.second) << ") ";
                cout << endl;
            };
        }

        /* Prepare and start timeout */
        params.timeout = make_shared<Timeout>(options_vars.count("timeout") ? seconds{ options_vars["timeout"].as<int>() } : 0s);

        /* Start the clock */
        params.start_time = steady_clock::now();

        if (options_vars.count("pattern-symmetries")) {
            auto gap_start_time = steady_clock::now();
            find_symmetries(argv[0], graphs.first, params.pattern_less_constraints, pattern_automorphism_group_size);
            was_given_automorphism_group = true;
            cout << "pattern_symmetry_time = " << duration_cast<milliseconds>(steady_clock::now() - gap_start_time).count() << endl;
        }

        if (was_given_automorphism_group)
            cout << "pattern_automorphism_group_size = " << pattern_automorphism_group_size << endl;

        auto result = solve_homomorphism_problem(graphs, params);

        /* Stop the clock. */
        auto overall_time = duration_cast<milliseconds>(steady_clock::now() - params.start_time);

        cout << "status = ";
        if (params.timeout->aborted())
            cout << "aborted";
        else if ((! result.mapping.empty()) || (params.count_solutions && result.solution_count > 0))
            cout << "true";
        else
            cout << "false";
        cout << endl;

        if (params.count_solutions)
            cout << "solution_count = " << result.solution_count << endl;

        cout << "nodes = " << result.nodes << endl;
        cout << "propagations = " << result.propagations << endl;

        if (! result.mapping.empty() && ! options_vars.count("print-all-solutions")) {
            cout << "mapping = ";
            for (auto v : result.mapping)
                cout << "(" << graphs.first.vertex_name(v.first) << " -> " << graphs.second.vertex_name(v.second) << ") ";
            cout << endl;
        }

        cout << "runtime = " << overall_time.count() << endl;

        for (const auto & s : result.extra_stats)
            cout << s << endl;

        return EXIT_SUCCESS;
    }
    catch (const GraphFileError & e) {
        cerr << "Error: " << e.what() << endl;
        return EXIT_FAILURE;
    }
    catch (const po::error & e) {
        cerr << "Error: " << e.what() << endl;
        cerr << "Try " << argv[0] << " --help" << endl;
        return EXIT_FAILURE;
    }
    catch (const exception & e) {
        cerr << "Error: " << e.what() << endl;
        return EXIT_FAILURE;
    }
}
