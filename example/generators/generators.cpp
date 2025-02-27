/*
 * A miniapp that demonstrates how to use event generators.
 *
 * The miniapp builds a simple model of a single cell, with one compartment
 * corresponding to the soma. The soma has a single synapse, to which two
 * event generators, one inhibitory, and one excitatory, are attached.
 */

#include <any>
#include <cassert>
#include <fstream>
#include <iomanip>
#include <iostream>

#include <nlohmann/json.hpp>

#include <arborio/label_parse.hpp>

#include <arbor/context.hpp>
#include <arbor/common_types.hpp>
#include <arbor/domain_decomposition.hpp>
#include <arbor/event_generator.hpp>
#include <arbor/load_balance.hpp>
#include <arbor/cable_cell.hpp>
#include <arbor/simple_sampler.hpp>
#include <arbor/recipe.hpp>
#include <arbor/simulation.hpp>

using arb::cell_gid_type;
using arb::cell_lid_type;
using arb::cell_size_type;
using arb::cell_member_type;
using arb::cell_kind;
using arb::time_type;

using namespace arborio::literals;

// Writes voltage trace as a json file.
void write_trace_json(const arb::trace_data<double>& trace);

class generator_recipe: public arb::recipe {
public:
    // There is just the one cell in the model
    cell_size_type num_cells() const override {
        return 1;
    }

    // Create cell with just a single compartment for the soma:
    //    soma diameter: 18.8 µm
    //    mechanisms: pas [default params]
    //    bulk resistivitiy: 100 Ω·cm [default]
    //    capacitance: 0.01 F/m² [default]
    //    synapses: 1 * expsyn
    arb::util::unique_any get_cell_description(cell_gid_type gid) const override {
        arb::segment_tree tree;
        double r = 18.8/2.0; // convert 18.8 μm diameter to radius
        tree.append(arb::mnpos, {0,0,-r,r}, {0,0,r,r}, 1);

        arb::label_dict labels;
        labels.set("soma", arb::reg::tagged(1));

        auto decor = arb::decor{}
            .paint("soma"_lab, arb::density("pas"))
            // Add one synapse at the soma.
            // This synapse will be the target for all events, from both
            // event_generators.
        .place(arb::mlocation{0, 0.5}, arb::synapse("expsyn"), "syn");

        return arb::cable_cell(tree, decor, labels);
    }

    cell_kind get_cell_kind(cell_gid_type gid) const override {
        assert(gid==0); // There is only one cell in the model
        return cell_kind::cable;
    }

    std::any get_global_properties(arb::cell_kind) const override {
        arb::cable_cell_global_properties gprop;
        gprop.default_parameters = arb::neuron_parameter_defaults;
        return gprop;
    }

    // Return two generators attached to the one cell.
    std::vector<arb::event_generator> event_generators(cell_gid_type gid) const override {
        assert(gid==0); // There is only one cell in the model

        using RNG = std::mt19937_64;

        auto hz_to_freq = [](double hz) { return hz*1e-3; };
        time_type t0 = 0;

        // Define frequencies and weights for the excitatory and inhibitory generators.
        double lambda_e =  hz_to_freq(500);
        double lambda_i =  hz_to_freq(20);
        double w_e =  0.001;
        double w_i = -0.005;

        // Make two event generators.
        std::vector<arb::event_generator> gens;

        // Add excitatory generator
        gens.push_back(
            arb::poisson_generator({"syn"},               // Target synapse index on cell `gid`
                                   w_e,                   // Weight of events to deliver
                                   t0,                    // Events start being delivered from this time
                                   lambda_e,              // Expected frequency (kHz)
                                   RNG(29562872)));       // Random number generator to use

        // Add inhibitory generator
        gens.emplace_back(
            arb::poisson_generator({"syn"}, w_i, t0, lambda_i,  RNG(86543891)));

        return gens;
    }

    std::vector<arb::probe_info> get_probes(cell_gid_type gid) const override {
        assert(gid==0);     // There is only one cell,

        // Measure membrane voltage at end of soma.
        arb::mlocation loc{0, 0.0};
        return {arb::cable_probe_membrane_voltage{loc}};
    }
};

int main() {
    // A distributed_context is required for distributed computation (e.g. MPI).
    // For this simple one-cell example, non-distributed context is suitable,
    // which is what we get with a default-constructed distributed_context.
    auto context = arb::make_context();

    // Create an instance of our recipe.
    generator_recipe recipe;

    // Construct the model.
    arb::simulation sim(recipe, context);

    // Set up the probe that will measure voltage in the cell.

    // The id of the only probe on the cell: the cell_member type points to (cell 0, probe 0)
    auto probeset_id = cell_member_type{0, 0};
    // The schedule for sampling is 10 samples every 1 ms.
    auto sched = arb::regular_schedule(0.1);
    // This is where the voltage samples will be stored as (time, value) pairs
    arb::trace_vector<double> voltage;
    // Now attach the sampler at probeset_id, with sampling schedule sched, writing to voltage
    sim.add_sampler(arb::one_probe(probeset_id), sched, arb::make_simple_sampler(voltage));

    // Run the simulation for 100 ms, with time steps of 0.01 ms.
    sim.run(100, 0.01);

    // Write the samples to a json file.
    write_trace_json(voltage.at(0));
}

void write_trace_json(const arb::trace_data<double>& trace) {
    std::string path = "./voltages.json";

    nlohmann::json json;
    json["name"] = "event_gen_demo";
    json["units"] = "mV";
    json["cell"] = "0.0";
    json["probe"] = "0";

    auto& jt = json["data"]["time"];
    auto& jy = json["data"]["voltage"];

    for (const auto& sample: trace) {
        jt.push_back(sample.t);
        jy.push_back(sample.v);
    }

    std::ofstream file(path);
    file << std::setw(1) << json << "\n";
}

