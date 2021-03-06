/*
    Copyright (c) 2016, Taiga Nomi
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
    names of its contributors may be used to endorse or promote products
    derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#pragma once

#include <vector>
#include <unordered_map>

#include "tiny_dnn/util/util.h"
#include "tiny_dnn/layers/layer.h"
#include "tiny_dnn/optimizers/optimizer.h"

namespace tiny_dnn {

/** basic class of various network types (sequential, multi-in/multi-out).
 *
 * this class holds list of pointer of Node, and provides entry point of
 * forward / backward operations.
 * Node is a computational unit of tiny-dnn (for example, convolution).
 * Currently 2 kinds of implementation are available: sequential and graph.
 *
 * Nodes can accept lvalue, rvalue and shared_ptr forms of node.
 * If given type is rvalue or shared_ptr, nodes create shared_ptr<node> to keep
 * given node alive. If given type is lvalue, tiny-dnn holds raw-pointer only
 * (to avoid double-free).
 *
 *     sequential s;
 *     s.add(fc<tan_h>(100, 200));                   // rvalue, moved into nodes
 *
 *     s.add(std::make_shared<fc<tan_h>>(200, 100)); // shared_ptr, shared by nodes
 *
 *     fc<softmax> out(100, 10);
 *     s.add(out);                                   // lvalue, hold raw-pointer only
 *
 **/
class nodes {
 public:
     typedef std::vector<layerptr_t>::iterator iterator;
     typedef std::vector<layerptr_t>::const_iterator const_iterator;

    /**
     * propagate gradient
     * @param first        : gradient of cost function(dE/dy)
     * @param worker_index : id of worker-task
     **/
    virtual
    void backward(const std::vector<tensor_t>& first) = 0;

    /**
     * @param first input  : data vectors
     * @param worker_index : id of worker-task
     **/
    virtual
    std::vector<tensor_t> forward(const std::vector<tensor_t>& first) = 0; // NOLINT

    /**
     * update weights and clear all gradients
     **/
    virtual
    void update_weights(optimizer *opt, int batch_size) {
        for (auto l : nodes_) {
            l->update_weight(opt, batch_size);
        }
    }

    /**
     * setup all weights, must be called before forward/backward
     **/
    virtual void setup(bool reset_weight) {
        for (auto l : nodes_) {
            l->setup(reset_weight);
        }
    }

    void clear_grads() {
        for (auto l : nodes_) {
            l->clear_grads();
        }
    }

    size_t size() const { return nodes_.size(); }
    iterator begin() { return nodes_.begin(); }
    iterator end() { return nodes_.end(); }
    const_iterator begin() const { return nodes_.begin(); }
    const_iterator end() const { return nodes_.end(); }
    layer* operator[] (size_t index) { return nodes_[index]; }
    const layer* operator[] (size_t index) const { return nodes_[index]; }
    cnn_size_t in_data_size() const { return nodes_.front()->in_data_size(); }
    cnn_size_t out_data_size() const { return nodes_.back()->out_data_size(); }

    template <typename T>
    const T& at(size_t index) const {
        const T* v = dynamic_cast<const T*>(nodes_[index]);
        if (v) return *v;
        throw nn_error("failed to cast");
    }

    template <typename T>
    T& at(size_t index) {
        T* v = dynamic_cast<T*>(nodes_[index]);
        if (v) return *v;
        throw nn_error("failed to cast");
    }

    // @todo: multiple output
    virtual float_t target_value_min(int out_channel = 0) const {
        CNN_UNREFERENCED_PARAMETER(out_channel);
        return nodes_.back()->out_value_range().first;
    }

    virtual float_t target_value_max(int out_channel = 0) const {
        CNN_UNREFERENCED_PARAMETER(out_channel);
        return nodes_.back()->out_value_range().second;
    }

    virtual void save(std::ostream& os) const { // NOLINT
        for (auto& l : nodes_) {
            l->save(os);
        }
    }

    virtual void load(std::istream& is) { // NOLINT
        setup(false);
        for (auto& l : nodes_) {
            l->load(is);
        }
    }

    virtual void load(const std::vector<float_t>& vec) {
        int idx = 0;
        setup(false);
        for (auto& l : nodes_) {
            l->load(vec, idx);
        }
    }

    void label2vec(const label_t* t, cnn_size_t num, std::vector<vec_t> *vec) const {
        cnn_size_t outdim = out_data_size();

        vec->reserve(num);
        for (cnn_size_t i = 0; i < num; i++) {
            assert(t[i] < outdim);
            vec->emplace_back(outdim, target_value_min());
            vec->back()[t[i]] = target_value_max();
        }
    }

 protected:
    template <typename T>
    void push_back(T&& node) {
        push_back_impl(std::forward<T>(node),
                       typename std::is_rvalue_reference<decltype(node)>::type()); // NOLINT
    }

    template <typename T>
    void push_back(std::shared_ptr<T> node) {
        own_nodes_.push_back(node);
        nodes_.push_back(own_nodes_.back().get());
    }

    // transform indexing so that it's more suitable for per-layer operations
    // input:  [sample][channel][feature]
    // output: [channel][sample][feature]
    std::vector<tensor_t> reorder_for_layerwise_processing(const std::vector<tensor_t>& input) {
        const cnn_size_t sample_count = input.size();
        const cnn_size_t channel_count = input[0].size();

        // @todo we could perhaps pass pointers to underlying vec_t objects, in order to avoid copying
        std::vector<tensor_t> output(channel_count, tensor_t(sample_count));

        for (cnn_size_t sample = 0; sample < sample_count; ++sample) {
            assert(input[sample].size() == channel_count);
            for (cnn_size_t channel = 0; channel < channel_count; ++channel) {
                output[channel][sample] = input[sample][channel];
            }
        }

        return output;
    }

 protected:
    template <typename T>
    void push_back_impl(T&& node, std::true_type) {  // is_rvalue_reference
        own_nodes_.push_back(std::make_shared<
            typename std::remove_reference<T>::type>(std::forward<T>(node)));
        nodes_.push_back(own_nodes_.back().get());
    }

    template <typename T>
    void push_back_impl(T&& node, std::false_type) {
        nodes_.push_back(&node);
    }

    /* Nodes which this class has ownership */
    std::vector<std::shared_ptr<layer>> own_nodes_;
    /* List of all nodes which includes own_nodes */
    std::vector<layerptr_t> nodes_;
};

/**
 * single-input, single-output feedforward network
 **/
class sequential : public nodes {
 public:
    void backward(const std::vector<tensor_t>& first) override {

        const std::vector<tensor_t> reordered_grad = reorder_for_layerwise_processing(first);
        assert(reordered_grad.size() == 1);

        nodes_.back()->set_out_grads({ reordered_grad[0] });

        for (auto l = nodes_.rbegin(); l != nodes_.rend(); l++) {
            (*l)->backward();
        }
    }

    std::vector<tensor_t> forward(const std::vector<tensor_t>& first) override {

        const std::vector<tensor_t> reordered_data = reorder_for_layerwise_processing(first);
        assert(reordered_data.size() == 1);

        nodes_.front()->set_in_data({ reordered_data[0] });

        for (auto l : nodes_) {
            if (l->initialized()) {
                l->forward();
            } else {
                throw nn_error("Layer " + l->layer_type() + " not initialized.");
            }
        }

        const std::vector<tensor_t> out = nodes_.back()->output();

        return normalize_out(out);
    }

    template <typename T>
    void add(T&& layer) {
        push_back(std::forward<T>(layer));

        if (nodes_.size() != 1) {
            auto head = nodes_[nodes_.size()-2];
            auto tail = nodes_[nodes_.size()-1];
            connect(head, tail, 0, 0);
            auto out = head->outputs();
            auto in = tail->inputs();
        }
        check_connectivity();
    }

    void check_connectivity() {
        for (cnn_size_t i = 0; i < nodes_.size() - 1; i++) {
            auto out = nodes_[i]->outputs();
            auto in = nodes_[i+1]->inputs();

            if (out[0] != in[0]) {
                throw nn_error("");
            }
        }
    }

private:
    std::vector<tensor_t> normalize_out(const std::vector<tensor_t>& out)
    {
        // normalize indexing back to [sample][layer][feature]
        std::vector<tensor_t> normalized_output;

        const cnn_size_t sample_count = out[0].size();
        normalized_output.resize(sample_count, tensor_t(1));

        for (cnn_size_t sample = 0; sample < sample_count; ++sample) {
            normalized_output[sample][0] = out[0][sample];
        }

        return normalized_output;
    }
};

/**
 * generic graph network
 * @todo not implemented
 **/
class graph : public nodes {
 public:
    void backward(const std::vector<tensor_t>& out_grad) override {

        cnn_size_t output_channel_count = out_grad[0].size();

        if (output_channel_count != output_layers_.size()) {
            throw nn_error("input size mismatch");
        }

        const std::vector<tensor_t> reordered_grad = reorder_for_layerwise_processing(out_grad);
        assert(reordered_grad.size() == output_channel_count);

        for (cnn_size_t i = 0; i < output_channel_count; i++) {
            output_layers_[i]->set_out_grads({ reordered_grad[i] });
        }

        for (auto l = nodes_.rbegin(); l != nodes_.rend(); l++) {
            (*l)->backward();
        }
    }

    std::vector<tensor_t> forward(const std::vector<tensor_t>& in_data) override {

        cnn_size_t input_data_channel_count = in_data[0].size();

        if (input_data_channel_count != input_layers_.size()) {
            throw nn_error("input size mismatch");
        }

        const std::vector<tensor_t> reordered_data = reorder_for_layerwise_processing(in_data);
        assert(reordered_data.size() == input_data_channel_count);

        for (cnn_size_t channel_index = 0; channel_index < input_data_channel_count; channel_index++) {
            input_layers_[channel_index]->set_in_data({ reordered_data[channel_index] });
        }

        for (auto l : nodes_) {
            l->forward();
        }
        return merge_outs();
    }

    void construct(const std::vector<layerptr_t>& input,
                   const std::vector<layerptr_t>& output) {
        std::vector<layerptr_t> sorted;
        std::vector<nodeptr_t> input_nodes(input.begin(), input.end());
        std::unordered_map<node*, std::vector<uint8_t>> removed_edge;

        // topological-sorting
        while (!input_nodes.empty()) {
            sorted.push_back(dynamic_cast<layerptr_t>(input_nodes.back()));
            input_nodes.pop_back();

            layerptr_t curr = sorted.back();
            std::vector<node*> next = curr->next_nodes();

            for (size_t i = 0; i < next.size(); i++) {
                if (!next[i]) continue;
                // remove edge between next[i] and current
                if (removed_edge.find(next[i]) == removed_edge.end()) {
                    removed_edge[next[i]] =
                        std::vector<uint8_t>(next[i]->prev_nodes().size(), 0);
                }

                std::vector<uint8_t>& removed = removed_edge[next[i]];
                removed[find_index(next[i]->prev_nodes(), curr)] = 1;

                if (std::all_of(removed.begin(), removed.end(), [](uint8_t x) {
                        return x == 1; })) {
                    input_nodes.push_back(next[i]);
                }
            }
        }

        for (auto& n : sorted) {
            nodes_.push_back(n);
        }

        input_layers_ = input;
        output_layers_ = output;

        setup(false);
    }

 private:

     // normalize indexing back to [sample][layer][feature]
     std::vector<tensor_t> merge_outs() {
         std::vector<tensor_t> merged;
         cnn_size_t output_channel_count = output_layers_.size();
         for (cnn_size_t output_channel = 0; output_channel < output_channel_count; ++output_channel) {
             std::vector<tensor_t> out = output_layers_[output_channel]->output();

             cnn_size_t sample_count = out[0].size();
             if (output_channel == 0) {
                 assert(merged.empty());
                 merged.resize(sample_count, tensor_t(output_channel_count));
             }

             assert(merged.size() == sample_count);

             for (cnn_size_t sample = 0; sample < sample_count; ++sample) {
                 merged[sample][output_channel] = out[0][sample];
             }
         }
         return merged;
     }

    cnn_size_t find_index(const std::vector<node*>& nodes,
                          layerptr_t target) {
        for (cnn_size_t i = 0; i < nodes.size(); i++) {
            if (nodes[i] == static_cast<node*>(&*target)) return i;
        }
        throw nn_error("invalid connection");
    }
    std::vector<layerptr_t> input_layers_;
    std::vector<layerptr_t> output_layers_;
};

}  // namespace tiny_dnn
