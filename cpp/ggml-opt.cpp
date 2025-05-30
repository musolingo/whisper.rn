#include "ggml-opt.h"

#include "ggml.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml-impl.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cinttypes>
#include <map>
#include <random>
#include <vector>

struct wsp_ggml_opt_dataset {
    struct wsp_ggml_context   * ctx    = nullptr;
    wsp_ggml_backend_buffer_t   buf    = nullptr;
    struct wsp_ggml_tensor    * data   = nullptr;
    struct wsp_ggml_tensor    * labels = nullptr;

    int64_t ndata       = -1;
    int64_t ndata_shard = -1;
    size_t  nbs_data    = -1;
    size_t  nbs_labels  = -1;

    std::vector<int64_t> permutation;
};

struct wsp_ggml_opt_context {
    wsp_ggml_backend_sched_t    backend_sched        = nullptr;
    wsp_ggml_cgraph           * allocated_graph      = nullptr;
    wsp_ggml_cgraph           * allocated_graph_copy = nullptr;
    struct wsp_ggml_context   * ctx_static           = nullptr;
    struct wsp_ggml_context   * ctx_static_cpu       = nullptr;
    struct wsp_ggml_context   * ctx_compute          = nullptr;
    struct wsp_ggml_context   * ctx_copy             = nullptr;
    wsp_ggml_backend_buffer_t   buf_static           = nullptr;
    wsp_ggml_backend_buffer_t   buf_static_cpu       = nullptr;
    std::mt19937            rng;

    struct wsp_ggml_tensor * inputs  = nullptr;
    struct wsp_ggml_tensor * outputs = nullptr;
    struct wsp_ggml_tensor * labels  = nullptr;

    struct wsp_ggml_tensor * loss     = nullptr;
    struct wsp_ggml_tensor * pred     = nullptr;
    struct wsp_ggml_tensor * ncorrect = nullptr;

    struct wsp_ggml_cgraph * gf      = nullptr;
    struct wsp_ggml_cgraph * gb_grad = nullptr;
    struct wsp_ggml_cgraph * gb_opt  = nullptr;

    int64_t iter               = 1;
    int32_t opt_period         = 1;
    int32_t opt_i              = 0;
    bool    loss_per_datapoint = false;

    wsp_ggml_opt_get_optimizer_params get_opt_pars = nullptr;
    void * get_opt_pars_ud                     = nullptr;
    struct wsp_ggml_tensor * adamw_params          = nullptr;
};

struct wsp_ggml_opt_result {
    int64_t              ndata    = 0;
    std::vector<float>   loss;
    std::vector<int32_t> pred;
    int64_t              ncorrect = 0;

    int64_t opt_period         = -1;
    bool    loss_per_datapoint = false;
};

// ====== Dataset ======

wsp_ggml_opt_dataset_t wsp_ggml_opt_dataset_init(int64_t ne_datapoint, int64_t ne_label, int64_t ndata, int64_t ndata_shard) {
    WSP_GGML_ASSERT(ne_datapoint >  0);
    WSP_GGML_ASSERT(ne_label     >= 0);
    WSP_GGML_ASSERT(ndata        >  0);
    WSP_GGML_ASSERT(ndata_shard  >  0);

    wsp_ggml_opt_dataset_t result = new wsp_ggml_opt_dataset;
    result->ndata       = ndata;
    result->ndata_shard = ndata_shard;

    {
        struct wsp_ggml_init_params params = {
            /*.mem_size   =*/ 2*wsp_ggml_tensor_overhead(),
            /*.mem_buffer =*/ nullptr,
            /*.no_alloc   =*/ true,
        };
        result->ctx = wsp_ggml_init(params);
    }

    result->data = wsp_ggml_new_tensor_2d(result->ctx, WSP_GGML_TYPE_F32, ne_datapoint, ndata);
    result->nbs_data = wsp_ggml_nbytes(result->data) * ndata_shard/ndata;

    if (ne_label > 0) {
        result->labels = wsp_ggml_new_tensor_2d(result->ctx, WSP_GGML_TYPE_F32, ne_label, ndata);
        result->nbs_labels = wsp_ggml_nbytes(result->labels) * ndata_shard/ndata;
    } else {
        result->labels = nullptr;
        result->nbs_labels = 0;
    }

    result->buf = wsp_ggml_backend_alloc_ctx_tensors_from_buft(result->ctx, wsp_ggml_backend_cpu_buffer_type());

    const int64_t nshards = ndata/ndata_shard;
    result->permutation.resize(nshards);
    for (int64_t i = 0; i < nshards; ++i) {
        result->permutation[i] = i;
    }
    return result;
}

void wsp_ggml_opt_dataset_free(wsp_ggml_opt_dataset_t dataset) {
    wsp_ggml_backend_buffer_free(dataset->buf);
    wsp_ggml_free(dataset->ctx);
    delete dataset;
}

struct wsp_ggml_tensor * wsp_ggml_opt_dataset_data(wsp_ggml_opt_dataset_t dataset) {
    return dataset->data;
}

struct wsp_ggml_tensor * wsp_ggml_opt_dataset_labels(wsp_ggml_opt_dataset_t dataset) {
    return dataset->labels;
}

void wsp_ggml_opt_dataset_shuffle(wsp_ggml_opt_context_t opt_ctx, wsp_ggml_opt_dataset_t dataset, int64_t idata) {
    WSP_GGML_ASSERT(idata <= dataset->ndata);

    if (idata < 0) {
        std::shuffle(dataset->permutation.begin(), dataset->permutation.end(), opt_ctx->rng);
        return;
    }

    WSP_GGML_ASSERT(idata % dataset->ndata_shard == 0);
    const int64_t ishard_max = idata / dataset->ndata_shard;
    std::shuffle(dataset->permutation.begin(), dataset->permutation.begin() + ishard_max, opt_ctx->rng);
}

void wsp_ggml_opt_dataset_get_batch(wsp_ggml_opt_dataset_t dataset, struct wsp_ggml_tensor * data_batch, struct wsp_ggml_tensor * labels_batch, int64_t ibatch) {
    WSP_GGML_ASSERT(   data_batch && wsp_ggml_is_contiguous(data_batch));
    WSP_GGML_ASSERT(!labels_batch || wsp_ggml_is_contiguous(labels_batch));
    WSP_GGML_ASSERT((labels_batch == nullptr) == (dataset->labels == nullptr));

    const size_t nb_data_batch = wsp_ggml_nbytes(data_batch);
    WSP_GGML_ASSERT(nb_data_batch % dataset->nbs_data == 0);
    const int64_t shards_per_batch = nb_data_batch / dataset->nbs_data;

    if (labels_batch) {
        const size_t nb_labels_batch = wsp_ggml_nbytes(labels_batch);
        WSP_GGML_ASSERT(nb_labels_batch == shards_per_batch*dataset->nbs_labels);
    }

    WSP_GGML_ASSERT((ibatch + 1)*shards_per_batch <= int64_t(dataset->permutation.size()));

    for (int64_t ishard_batch = 0; ishard_batch < shards_per_batch; ++ishard_batch) {
        const int64_t ishard = dataset->permutation[ibatch*shards_per_batch + ishard_batch];

        const char * ptr_data = (const char *) dataset->data->data + ishard*dataset->nbs_data;
        wsp_ggml_backend_tensor_set(data_batch, ptr_data, ishard_batch*dataset->nbs_data, dataset->nbs_data);

        if (!labels_batch) {
            continue;
        }

        const char * ptr_labels = (const char *) dataset->labels->data + ishard*dataset->nbs_labels;
        wsp_ggml_backend_tensor_set(labels_batch, ptr_labels, ishard_batch*dataset->nbs_labels, dataset->nbs_labels);
    }
}

// ====== Model / Context ======

struct wsp_ggml_opt_optimizer_params wsp_ggml_opt_get_default_optimizer_params(void * userdata) {
    WSP_GGML_UNUSED(userdata);

    wsp_ggml_opt_optimizer_params result;

    result.adamw.alpha = 0.001f;
    result.adamw.beta1 = 0.9f;
    result.adamw.beta2 = 0.999f;
    result.adamw.eps   = 1e-8f;
    result.adamw.wd    = 0.0f;

    return result;
}

struct wsp_ggml_opt_params wsp_ggml_opt_default_params(
        wsp_ggml_backend_sched_t      backend_sched,
        struct wsp_ggml_context     * ctx_compute,
        struct wsp_ggml_tensor      * inputs,
        struct wsp_ggml_tensor      * outputs,
        enum wsp_ggml_opt_loss_type   loss_type) {
    return {
        /*backend_sched   =*/ backend_sched,
        /*ctx_compute     =*/ ctx_compute,
        /*inputs          =*/ inputs,
        /*logits          =*/ outputs,
        /*loss_type       =*/ loss_type,
        /*build_type      =*/ WSP_GGML_OPT_BUILD_TYPE_OPT,
        /*opt_period      =*/ 1,
        /*get_opt_pars    =*/ wsp_ggml_opt_get_default_optimizer_params,
        /*get_opt_pars_ud =*/ nullptr,
    };
}

static wsp_ggml_tensor * map_tensor(std::map<wsp_ggml_tensor *, wsp_ggml_tensor *> & tensor_map, wsp_ggml_context * ctx, wsp_ggml_tensor * tensor) {
    if (!tensor) {
        return nullptr;
    }

    if (tensor_map.find(tensor) != tensor_map.end()) {
        return tensor_map[tensor];
    }

    wsp_ggml_tensor * new_tensor = wsp_ggml_dup_tensor(ctx, tensor);
    tensor_map[tensor] = new_tensor;

    new_tensor->op = tensor->op;
    for (int i = 0; i < WSP_GGML_MAX_DIMS; i++) {
        new_tensor->nb[i] = tensor->nb[i];
    }
    new_tensor->flags = tensor->flags;
    memcpy(new_tensor->op_params, tensor->op_params, sizeof(tensor->op_params));
    strcpy(new_tensor->name, tensor->name);
    new_tensor->data = tensor->data;
    new_tensor->buffer = tensor->buffer;
    new_tensor->extra = tensor->extra;
    new_tensor->view_offs = tensor->view_offs;
    new_tensor->view_src = map_tensor(tensor_map, ctx, tensor->view_src);
    for (int i = 0; i < WSP_GGML_MAX_SRC; i++) {
        new_tensor->src[i] = map_tensor(tensor_map, ctx, tensor->src[i]);
    }

    return new_tensor;
}

static wsp_ggml_cgraph * dup_graph(wsp_ggml_context * ctx, wsp_ggml_cgraph * src) {
    std::map<wsp_ggml_tensor *, wsp_ggml_tensor *> tensor_map;

    wsp_ggml_cgraph * dst = wsp_ggml_new_graph_custom(ctx, src->size, /*grads =*/ true);

    for (int i = 0; i < src->n_leafs; i++) {
        wsp_ggml_build_forward_expand(dst, map_tensor(tensor_map, ctx, src->leafs[i]));
    }
    WSP_GGML_ASSERT(dst->n_leafs == src->n_leafs);
    for (int i = 0; i < src->n_nodes; i++) {
        wsp_ggml_build_forward_expand(dst, map_tensor(tensor_map, ctx, src->nodes[i]));
    }
    WSP_GGML_ASSERT(dst->n_nodes == src->n_nodes);
    for (int i = 0; i < src->n_nodes; ++i) {
        const size_t igrad_src = wsp_ggml_hash_find(&src->visited_hash_set, src->nodes[i]);
        const size_t igrad_dst = wsp_ggml_hash_find(&dst->visited_hash_set, dst->nodes[i]);

        WSP_GGML_ASSERT(igrad_src != WSP_GGML_HASHSET_FULL);
        WSP_GGML_ASSERT(wsp_ggml_bitset_get(src->visited_hash_set.used, igrad_src));
        WSP_GGML_ASSERT(igrad_dst != WSP_GGML_HASHSET_FULL);
        WSP_GGML_ASSERT(wsp_ggml_bitset_get(dst->visited_hash_set.used, igrad_dst));

        dst->grads[igrad_dst]     = src->grads[igrad_src];
        dst->grad_accs[igrad_dst] = src->grad_accs[igrad_src];
    }

    return dst;
}

static void wsp_ggml_opt_alloc_graph(wsp_ggml_opt_context_t opt_ctx, wsp_ggml_cgraph * graph) {
    WSP_GGML_ASSERT(graph);
    if (opt_ctx->allocated_graph == graph) {
        return;
    }

    wsp_ggml_backend_sched_reset(opt_ctx->backend_sched); // clear allocation of previous graph

    {
        wsp_ggml_init_params params = {
            /*.mem_size   =*/ wsp_ggml_tensor_overhead() * WSP_GGML_DEFAULT_GRAPH_SIZE,
            /*.mem_buffer =*/ nullptr,
            /*.no_alloc   =*/ true,
        };
        wsp_ggml_free(opt_ctx->ctx_copy);
        opt_ctx->ctx_copy = wsp_ggml_init(params);
    }

    opt_ctx->allocated_graph_copy = dup_graph(opt_ctx->ctx_copy, graph);

    wsp_ggml_backend_sched_alloc_graph(opt_ctx->backend_sched, opt_ctx->allocated_graph_copy);
    opt_ctx->allocated_graph = graph;
}

wsp_ggml_opt_context_t wsp_ggml_opt_init(struct wsp_ggml_opt_params params) {
    wsp_ggml_opt_context_t result = new struct wsp_ggml_opt_context;
    result->backend_sched   = params.backend_sched;
    result->ctx_compute     = params.ctx_compute;
    result->inputs          = params.inputs;
    result->outputs         = params.outputs;
    result->opt_period      = params.opt_period;
    result->get_opt_pars    = params.get_opt_pars;
    result->get_opt_pars_ud = params.get_opt_pars_ud;

    WSP_GGML_ASSERT(result->inputs->data && "the inputs must be allocated statically");
    WSP_GGML_ASSERT(result->opt_period >= 1);

    const bool accumulate = params.build_type == WSP_GGML_OPT_BUILD_TYPE_GRAD ||
        (params.build_type == WSP_GGML_OPT_BUILD_TYPE_OPT && result->opt_period > 1);

    wsp_ggml_set_input(result->inputs);
    wsp_ggml_set_output(result->outputs);

    result->gf = wsp_ggml_new_graph_custom(result->ctx_compute, WSP_GGML_DEFAULT_GRAPH_SIZE, /*grads =*/ true); // Forward pass.
    wsp_ggml_build_forward_expand(result->gf, result->outputs);

    int n_param = 0;
    for (int i = 0; i < result->gf->n_nodes; ++i) {
        if (result->gf->nodes[i]->flags & WSP_GGML_TENSOR_FLAG_PARAM) {
            n_param++;
        }
    }

    {
        // The static context is used for:
        //   - gradients (1 tensor per param if using gradient accumulation)
        //   - optimizer momenta (2 tensors per param)
        //   - labels
        //   - loss + its gradient (up to 5 tensors)
        //   - pred
        //   - ncorrect (2 tensors).
        const size_t tensors_per_param = (accumulate ? 1 : 0) + (params.build_type == WSP_GGML_OPT_BUILD_TYPE_OPT ? 2 : 0);
        const size_t size_meta = (tensors_per_param*n_param + 9) * wsp_ggml_tensor_overhead();
        struct wsp_ggml_init_params params = {
            /*.mem_size   =*/ size_meta,
            /*.mem_buffer =*/ nullptr,
            /*.no_alloc   =*/ true,
        };
        result->ctx_static = wsp_ggml_init(params);
    }
    {
        // The static cpu context is used for:
        //   - optimizer parameters (1 for the entire context)
        const size_t size_meta = 1 * wsp_ggml_tensor_overhead();
        struct wsp_ggml_init_params params = {
            /*.mem_size   =*/ size_meta,
            /*.mem_buffer =*/ nullptr,
            /*.no_alloc   =*/ true,
        };
        result->ctx_static_cpu = wsp_ggml_init(params);
    }


    switch (params.loss_type) {
        case WSP_GGML_OPT_LOSS_TYPE_MEAN: {
            result->loss = wsp_ggml_sum(result->ctx_static, result->outputs);
            wsp_ggml_set_name(result->loss, "loss_sum");
            const float scale = 1.0f / (result->opt_period * wsp_ggml_nelements(result->outputs));
            result->loss = wsp_ggml_scale(result->ctx_static, result->loss, scale);
            wsp_ggml_set_name(result->loss, "loss_mean");
            result->loss_per_datapoint = true;
            break;
        }
        case WSP_GGML_OPT_LOSS_TYPE_SUM: {
            result->loss = wsp_ggml_sum(result->ctx_static, result->outputs);
            wsp_ggml_set_name(result->loss, "loss_sum");
            result->loss_per_datapoint = false;
            break;
        }
        case WSP_GGML_OPT_LOSS_TYPE_CROSS_ENTROPY: {
            result->labels = wsp_ggml_dup_tensor(result->ctx_static, result->outputs);
            wsp_ggml_set_input(result->labels);
            wsp_ggml_set_name(result->labels, "labels");
            result->loss = wsp_ggml_cross_entropy_loss(result->ctx_static, result->outputs, result->labels);
            wsp_ggml_set_name(result->loss, "loss_cross_entropy");
            if (result->opt_period > 1) {
                result->loss = wsp_ggml_scale(result->ctx_static, result->loss, 1.0f / result->opt_period);
                wsp_ggml_set_name(result->loss, "loss_cross_entropy_scaled");
            }
            result->loss_per_datapoint = true;
            break;
        }
        case WSP_GGML_OPT_LOSS_TYPE_MEAN_SQUARED_ERROR: {
            result->labels = wsp_ggml_dup_tensor(result->ctx_static, result->outputs);
            wsp_ggml_set_input(result->labels);
            wsp_ggml_set_name(result->labels, "labels");
            result->loss = wsp_ggml_sub(result->ctx_static, result->outputs, result->labels);
            wsp_ggml_set_name(result->loss, "loss_error");
            result->loss = wsp_ggml_sqr(result->ctx_static, result->loss);
            wsp_ggml_set_name(result->loss, "loss_squared_error");
            result->loss = wsp_ggml_sum(result->ctx_static, result->loss);
            wsp_ggml_set_name(result->loss, "loss_sum_squared_error");
            const float scale = 1.0f / (result->opt_period * wsp_ggml_nelements(result->outputs));
            result->loss = wsp_ggml_scale(result->ctx_static, result->loss, scale);
            wsp_ggml_set_name(result->loss, "loss_mean_squared_error");
            result->loss_per_datapoint = true;
            break;
        }
    }
    wsp_ggml_set_output(result->loss);
    wsp_ggml_set_loss(result->loss);
    wsp_ggml_build_forward_expand(result->gf, result->loss);

    result->pred = wsp_ggml_argmax(result->ctx_static, result->outputs);
    wsp_ggml_set_name(result->pred, "pred");
    wsp_ggml_set_output(result->pred);
    wsp_ggml_build_forward_expand(result->gf, result->pred);

    if (result->labels) {
        result->ncorrect = wsp_ggml_count_equal(result->ctx_static, result->pred, wsp_ggml_argmax(result->ctx_static, result->labels));
        wsp_ggml_set_name(result->ncorrect, "ncorrect");
        wsp_ggml_set_output(result->ncorrect);
        wsp_ggml_build_forward_expand(result->gf, result->ncorrect);
    } else {
        result->ncorrect = nullptr;
    }

    if (params.build_type == WSP_GGML_OPT_BUILD_TYPE_FORWARD) {
        result->buf_static = wsp_ggml_backend_alloc_ctx_tensors(result->ctx_static, wsp_ggml_backend_sched_get_backend(result->backend_sched, 0));
        return result;
    }

    // gb_grad == graph backward gradients, forward pass, then backward pass to calculate gradients.
    result->gb_grad = wsp_ggml_graph_dup(result->ctx_compute, result->gf);
    wsp_ggml_build_backward_expand(result->ctx_static, result->ctx_compute, result->gb_grad, accumulate);

    if (params.build_type == WSP_GGML_OPT_BUILD_TYPE_GRAD) {
        result->buf_static = wsp_ggml_backend_alloc_ctx_tensors(result->ctx_static, wsp_ggml_backend_sched_get_backend(result->backend_sched, 0));
        wsp_ggml_graph_reset(result->gb_grad);
        return result;
    }

    WSP_GGML_ASSERT(params.build_type == WSP_GGML_OPT_BUILD_TYPE_OPT);

    // gb_opt == graph backward optimize, forward pass, then backward pass to calculate gradients, then optimizer step.
    result->gb_opt = wsp_ggml_graph_dup(result->ctx_compute, result->gb_grad);

    result->adamw_params = wsp_ggml_new_tensor_1d(result->ctx_static_cpu, WSP_GGML_TYPE_F32, 7);
    wsp_ggml_set_input(result->adamw_params);
    wsp_ggml_set_name(result->adamw_params, "adamw_params");

    for (int i = result->gf->n_nodes-1; i >= 0; --i) {
        struct wsp_ggml_tensor * node = result->gb_opt->nodes[i];
        struct wsp_ggml_tensor * grad = wsp_ggml_graph_get_grad(result->gb_opt, node);

        if (node->flags & WSP_GGML_TENSOR_FLAG_PARAM) {
            struct wsp_ggml_tensor * m        = wsp_ggml_dup_tensor(result->ctx_static, node);
            struct wsp_ggml_tensor * v        = wsp_ggml_dup_tensor(result->ctx_static, node);
            struct wsp_ggml_tensor * opt_step = wsp_ggml_opt_step_adamw(result->ctx_compute, node, grad, m, v, result->adamw_params);
            wsp_ggml_build_forward_expand(result->gb_opt, opt_step);
        }
    }

    result->buf_static = wsp_ggml_backend_alloc_ctx_tensors(
        result->ctx_static, wsp_ggml_backend_sched_get_backend(result->backend_sched, 0));

    result->buf_static_cpu = wsp_ggml_backend_alloc_ctx_tensors_from_buft(result->ctx_static_cpu, wsp_ggml_backend_cpu_buffer_type());

    wsp_ggml_graph_reset(result->gb_opt);

    return result;
}

void wsp_ggml_opt_free(wsp_ggml_opt_context_t opt_ctx) {
    if (opt_ctx == nullptr) {
        return;
    }
    wsp_ggml_backend_buffer_free(opt_ctx->buf_static);
    wsp_ggml_backend_buffer_free(opt_ctx->buf_static_cpu);
    wsp_ggml_free(opt_ctx->ctx_static);
    wsp_ggml_free(opt_ctx->ctx_static_cpu);
    delete opt_ctx;
}

void wsp_ggml_opt_reset(wsp_ggml_opt_context_t opt_ctx, bool optimizer) {
    if (optimizer) {
        wsp_ggml_graph_reset(opt_ctx->gb_opt);
        opt_ctx->iter = 1;
    } else {
        wsp_ggml_graph_reset(opt_ctx->gb_grad);
    }
}

struct wsp_ggml_tensor * wsp_ggml_opt_inputs(wsp_ggml_opt_context_t opt_ctx) {
    return opt_ctx->inputs;
}

struct wsp_ggml_tensor * wsp_ggml_opt_outputs(wsp_ggml_opt_context_t opt_ctx) {
    return opt_ctx->outputs;
}

struct wsp_ggml_tensor * wsp_ggml_opt_labels(wsp_ggml_opt_context_t opt_ctx) {
    return opt_ctx->labels;
}

struct wsp_ggml_tensor * wsp_ggml_opt_loss(wsp_ggml_opt_context_t opt_ctx) {
    return opt_ctx->loss;
}

struct wsp_ggml_tensor * wsp_ggml_opt_pred(wsp_ggml_opt_context_t opt_ctx) {
    return opt_ctx->pred;
}

struct wsp_ggml_tensor * wsp_ggml_opt_ncorrect(wsp_ggml_opt_context_t opt_ctx) {
    return opt_ctx->ncorrect;
}

struct wsp_ggml_tensor * wsp_ggml_opt_grad_acc(wsp_ggml_opt_context_t opt_ctx, struct wsp_ggml_tensor * node) {
    return wsp_ggml_graph_get_grad_acc(opt_ctx->gb_opt, node);
}

// ====== Optimization Result ======

wsp_ggml_opt_result_t wsp_ggml_opt_result_init() {
    return new wsp_ggml_opt_result;
}

void wsp_ggml_opt_result_free(wsp_ggml_opt_result_t result) {
    delete result;
}

void wsp_ggml_opt_result_reset(wsp_ggml_opt_result_t result) {
    result->ndata = 0;
    result->loss.clear();
    result->pred.clear();
    result->ncorrect = 0;
}

void wsp_ggml_opt_result_ndata(wsp_ggml_opt_result_t result, int64_t * ndata) {
    *ndata = result->ndata;
}

void wsp_ggml_opt_result_loss(wsp_ggml_opt_result_t result, double * loss, double * unc) {
    const int64_t nbatches = result->loss.size(); // Number of physical batches.

    if (nbatches == 0) {
        *loss = 0.0;
        *unc  = NAN;
        return;
    }

    double sum         = 0.0;
    double sum_squared = 0.0;

    for (const float & loss : result->loss) {
        // If the loss is per datapoint it was scaled by 1.0f/opt_period for each physical batch.
        const float loss_scaled = result->loss_per_datapoint ? loss*result->opt_period : loss;
        sum         += loss_scaled;
        sum_squared += loss_scaled*loss_scaled;
    }

    const double mean = sum/nbatches;
    *loss = result->loss_per_datapoint ? mean : sum;

    if (!unc) {
        return;
    }

    if (nbatches < 2) {
        *unc = NAN;
        return;
    }

    const double var_sum = sum_squared/nbatches - mean*mean; // variance without Bessel's correction, i.e. nbatches/(nbatches-1)
    *unc = result->loss_per_datapoint ? sqrt(var_sum / (nbatches - 1)) : sqrt(var_sum * nbatches/(nbatches - 1));
}

void wsp_ggml_opt_result_pred(wsp_ggml_opt_result_t result, int32_t * pred) {
    for (size_t i = 0; i < result->pred.size(); ++i) {
        pred[i] = result->pred[i];
    }
}

void wsp_ggml_opt_result_accuracy(wsp_ggml_opt_result_t result, double * accuracy, double * unc) {
    *accuracy = result->ncorrect >= 0 ? double(result->ncorrect) / double(result->ndata) : NAN;

    if (!unc) {
        return;
    }

    *unc = result->ncorrect >= 0 && result->ndata >= 2 ?
        sqrt((*accuracy) * (1.0 - (*accuracy)) / double(result->ndata - 1)) : NAN;
}

// ====== Computation ======

static void wsp_ggml_opt_eval_graph(wsp_ggml_opt_context_t opt_ctx, wsp_ggml_cgraph * graph, wsp_ggml_opt_result * result) {
    if (graph != opt_ctx->gf) {
        struct wsp_ggml_opt_optimizer_params opt_pars = opt_ctx->get_opt_pars(opt_ctx->get_opt_pars_ud);

        WSP_GGML_ASSERT(opt_pars.adamw.alpha >  0.0f);
        WSP_GGML_ASSERT(opt_pars.adamw.beta1 >= 0.0f);
        WSP_GGML_ASSERT(opt_pars.adamw.beta1 <= 1.0f);
        WSP_GGML_ASSERT(opt_pars.adamw.beta2 >= 0.0f);
        WSP_GGML_ASSERT(opt_pars.adamw.beta2 <= 1.0f);
        WSP_GGML_ASSERT(opt_pars.adamw.eps   >= 0.0f);
        WSP_GGML_ASSERT(opt_pars.adamw.wd    >= 0.0f);
        WSP_GGML_ASSERT(opt_pars.adamw.wd    <= 1.0f);

        // beta1, beta2 after applying warmup
        const float beta1h = 1.0f/(1.0f - powf(opt_pars.adamw.beta1, opt_ctx->iter));
        const float beta2h = 1.0f/(1.0f - powf(opt_pars.adamw.beta2, opt_ctx->iter));

        float * adamw_par_data = wsp_ggml_get_data_f32(opt_ctx->adamw_params);
        adamw_par_data[0] = opt_pars.adamw.alpha;
        adamw_par_data[1] = opt_pars.adamw.beta1;
        adamw_par_data[2] = opt_pars.adamw.beta2;
        adamw_par_data[3] = opt_pars.adamw.eps;
        adamw_par_data[4] = opt_pars.adamw.wd;
        adamw_par_data[5] = beta1h;
        adamw_par_data[6] = beta2h;
    }

    wsp_ggml_opt_alloc_graph(opt_ctx, graph);
    wsp_ggml_backend_sched_graph_compute(opt_ctx->backend_sched, opt_ctx->allocated_graph_copy);
    opt_ctx->iter += opt_ctx->allocated_graph == opt_ctx->gb_opt;

    if (!result) {
        return;
    }

    if (result->ndata == 0) {
        result->loss_per_datapoint = opt_ctx->loss_per_datapoint;
        result->opt_period         = opt_ctx->opt_period;
    } else {
        WSP_GGML_ASSERT(result->loss_per_datapoint == opt_ctx->loss_per_datapoint);
        WSP_GGML_ASSERT(result->opt_period         == opt_ctx->opt_period);
    }

    const int64_t ndata = opt_ctx->outputs->ne[1];
    WSP_GGML_ASSERT(result->ndata == ndata*int64_t(result->loss.size()) && "varying batch size not supported");
    result->ndata += ndata;

    WSP_GGML_ASSERT(wsp_ggml_is_scalar(opt_ctx->loss));
    WSP_GGML_ASSERT(opt_ctx->loss->type == WSP_GGML_TYPE_F32);
    float loss;
    wsp_ggml_backend_tensor_get(opt_ctx->loss, &loss, 0, wsp_ggml_nbytes(opt_ctx->loss));
    result->loss.push_back(loss);

    WSP_GGML_ASSERT(opt_ctx->pred->type == WSP_GGML_TYPE_I32);
    std::vector<int32_t> pred(ndata);
    wsp_ggml_backend_tensor_get(opt_ctx->pred, pred.data(), 0, wsp_ggml_nbytes(opt_ctx->pred));
    result->pred.insert(result->pred.end(), pred.begin(), pred.end());

    if (!opt_ctx->labels || result->ncorrect < 0) {
        result->ncorrect = -1;
        return;
    }

    WSP_GGML_ASSERT(wsp_ggml_is_scalar(opt_ctx->ncorrect));
    WSP_GGML_ASSERT(opt_ctx->ncorrect->type == WSP_GGML_TYPE_I64);
    int64_t ncorrect;
    wsp_ggml_backend_tensor_get(opt_ctx->ncorrect, &ncorrect, 0, wsp_ggml_nbytes(opt_ctx->ncorrect));
    result->ncorrect += ncorrect;
}

void wsp_ggml_opt_forward(wsp_ggml_opt_context_t opt_ctx, wsp_ggml_opt_result * result) {
    wsp_ggml_opt_eval_graph(opt_ctx, opt_ctx->gf, result);
}

void wsp_ggml_opt_forward_backward(wsp_ggml_opt_context_t opt_ctx, wsp_ggml_opt_result * result) {
    if (opt_ctx->opt_period == 1) {
        wsp_ggml_opt_eval_graph(opt_ctx, opt_ctx->gb_opt, result);
        return;
    }

    const int32_t opt_i_next = (opt_ctx->opt_i + 1) % opt_ctx->opt_period;
    if (opt_i_next == 0) {
        wsp_ggml_opt_eval_graph(opt_ctx, opt_ctx->gb_opt, result);
        wsp_ggml_opt_reset(opt_ctx, /*optimizer =*/ false);
    } else {
        wsp_ggml_opt_eval_graph(opt_ctx, opt_ctx->gb_grad, result);
    }
    opt_ctx->opt_i = opt_i_next;
}

// ====== High-Level Functions ======

void wsp_ggml_opt_epoch(
        wsp_ggml_opt_context_t      opt_ctx,
        wsp_ggml_opt_dataset_t      dataset,
        wsp_ggml_opt_result_t       result_train,
        wsp_ggml_opt_result_t       result_eval,
        int64_t                 idata_split,
        wsp_ggml_opt_epoch_callback callback_train,
        wsp_ggml_opt_epoch_callback callback_eval) {
    struct wsp_ggml_tensor * inputs = wsp_ggml_opt_inputs(opt_ctx);
    struct wsp_ggml_tensor * labels = wsp_ggml_opt_labels(opt_ctx);
    struct wsp_ggml_tensor * data   = wsp_ggml_opt_dataset_data(dataset);
    WSP_GGML_ASSERT(data->ne[0] == inputs->ne[0]);

    const int64_t ndata       =   data->ne[1];
    const int64_t ndata_batch = inputs->ne[1];

    WSP_GGML_ASSERT(data->ne[1] % inputs->ne[1] == 0);
    const int64_t nbatches = ndata/ndata_batch;

    idata_split = idata_split < 0 ? ndata : idata_split;
    WSP_GGML_ASSERT(idata_split % ndata_batch == 0);
    const int64_t ibatch_split = idata_split / ndata_batch;

    int64_t ibatch = 0;
    int64_t t_loop_start = wsp_ggml_time_us();
    for (; ibatch < ibatch_split; ++ibatch) {
        wsp_ggml_opt_dataset_get_batch(dataset, inputs, labels, ibatch);
        wsp_ggml_opt_forward_backward(opt_ctx, result_train);
        if (callback_train) {
            callback_train(true, opt_ctx, dataset, result_train, ibatch+1, ibatch_split, t_loop_start);
        }
    }
    t_loop_start = wsp_ggml_time_us();
    for (; ibatch < nbatches; ++ibatch) {
        wsp_ggml_opt_dataset_get_batch(dataset, inputs, labels, ibatch);
        wsp_ggml_opt_forward(opt_ctx, result_eval);
        if (callback_eval) {
            callback_eval(false, opt_ctx, dataset, result_eval, ibatch+1-ibatch_split, nbatches-ibatch_split, t_loop_start);
        }
    }
}

void wsp_ggml_opt_epoch_callback_progress_bar(
        bool               train,
        wsp_ggml_opt_context_t opt_ctx,
        wsp_ggml_opt_dataset_t dataset,
        wsp_ggml_opt_result_t  result,
        int64_t            ibatch,
        int64_t            ibatch_max,
        int64_t            t_start_us) {
    fprintf(stderr, "%s[", train ? "train: " : "val:   ");

    constexpr int64_t bar_length = 25;
    for (int64_t j = 0; j < bar_length; ++j) {
        const int64_t ibatch_j = ibatch_max * j/bar_length;
        if (ibatch_j < ibatch) {
            fprintf(stderr, "=");
        } else if (ibatch_max * (j - 1)/bar_length < ibatch) {
            fprintf(stderr, ">");
        } else {
            fprintf(stderr, " ");
        }
    }

    const int64_t batch_size = wsp_ggml_opt_inputs(opt_ctx)->ne[1];
    const int64_t idata      = ibatch*batch_size;
    const int64_t idata_max  = ibatch_max*batch_size;

    double loss;
    double loss_unc;
    wsp_ggml_opt_result_loss(result, &loss, &loss_unc);

    double accuracy;
    double accuracy_unc;
    wsp_ggml_opt_result_accuracy(result, &accuracy, &accuracy_unc);

    const int64_t t_ibatch_us = wsp_ggml_time_us() - t_start_us;
    int64_t t_ibatch_s = t_ibatch_us / 1000000;
    const int64_t t_ibatch_h = t_ibatch_s / 3600;
    t_ibatch_s -= t_ibatch_h * 3600;
    const int64_t t_ibatch_m = t_ibatch_s / 60;
    t_ibatch_s -= t_ibatch_m * 60;

    const int64_t t_eta_us = t_ibatch_us * (ibatch_max - ibatch)/ibatch;
    int64_t t_eta_s = t_eta_us / 1000000;
    const int64_t t_eta_h = t_eta_s / 3600;
    t_eta_s -= t_eta_h * 3600;
    const int64_t t_eta_m = t_eta_s / 60;
    t_eta_s -= t_eta_m * 60;

    fprintf(stderr, "| data=%06" PRId64 "/%06" PRId64 ", loss=%.6lf+-%.6lf, accuracy=%.2lf+-%.2lf%%, "
            "t=%02" PRId64 ":%02" PRId64 ":%02" PRId64 ", ETA=%02" PRId64 ":%02" PRId64 ":%02" PRId64 "]\r",
            idata, idata_max, loss, loss_unc, 100.0*accuracy, 100.0*accuracy_unc,
            t_ibatch_h, t_ibatch_m, t_ibatch_s, t_eta_h, t_eta_m, t_eta_s);
    if (ibatch == ibatch_max) {
        fprintf(stderr, "\n");
    }
    fflush(stderr);

    WSP_GGML_UNUSED(dataset);
}

void wsp_ggml_opt_fit(
        wsp_ggml_backend_sched_t            backend_sched,
        wsp_ggml_context                  * ctx_compute,
        wsp_ggml_tensor                   * inputs,
        wsp_ggml_tensor                   * outputs,
        wsp_ggml_opt_dataset_t              dataset,
        enum wsp_ggml_opt_loss_type         loss_type,
        wsp_ggml_opt_get_optimizer_params   get_opt_pars,
        int64_t                         nepoch,
        int64_t                         nbatch_logical,
        float                           val_split,
        bool                            silent) {
    wsp_ggml_time_init();
    const int64_t t_start_us = wsp_ggml_time_us();

    const int64_t ndata           = wsp_ggml_opt_dataset_data(dataset)->ne[1];
    const int64_t nbatch_physical = inputs->ne[1];
    WSP_GGML_ASSERT(ndata          % nbatch_logical  == 0);
    WSP_GGML_ASSERT(nbatch_logical % nbatch_physical == 0);

    const int64_t opt_period       = nbatch_logical / nbatch_physical;
    const int64_t nbatches_logical = ndata / nbatch_logical;

    WSP_GGML_ASSERT(val_split >= 0.0f);
    WSP_GGML_ASSERT(val_split <  1.0f);
    const int64_t ibatch_split = int64_t(((1.0f - val_split) * nbatches_logical)) * opt_period; // train <-> val split index (physical)
    const int64_t idata_split  = ibatch_split * nbatch_physical;

    int64_t epoch = 1;

    wsp_ggml_opt_params params = wsp_ggml_opt_default_params(backend_sched, ctx_compute, inputs, outputs, loss_type);
    params.opt_period      = opt_period;
    params.get_opt_pars    = get_opt_pars;
    params.get_opt_pars_ud = &epoch;
    wsp_ggml_opt_context_t opt_ctx = wsp_ggml_opt_init(params);

    // Shuffling the data is generally useful but there is only a point if not all data is used in a single batch.
    if (nbatch_logical < ndata) {
        wsp_ggml_opt_dataset_shuffle(opt_ctx, dataset, -1); // Shuffle all data (train + validation).
    }

    wsp_ggml_opt_result_t result_train = wsp_ggml_opt_result_init();
    wsp_ggml_opt_result_t result_val   = wsp_ggml_opt_result_init();

    wsp_ggml_opt_epoch_callback epoch_callback = silent ? nullptr : wsp_ggml_opt_epoch_callback_progress_bar;

    for (; epoch <= nepoch; ++epoch) {
        if (nbatch_logical < idata_split) {
            wsp_ggml_opt_dataset_shuffle(opt_ctx, dataset, idata_split);
        }

        wsp_ggml_opt_result_reset(result_train);
        wsp_ggml_opt_result_reset(result_val);

        if (!silent) {
            fprintf(stderr, "%s: epoch %04" PRId64 "/%04" PRId64 ":\n", __func__, epoch, nepoch);
        }
        wsp_ggml_opt_epoch(opt_ctx, dataset, result_train, result_val, idata_split, epoch_callback, epoch_callback);
        if (!silent) {
            fprintf(stderr, "\n");
        }
    }

    if (!silent) {
        int64_t t_total_s = (wsp_ggml_time_us() - t_start_us) / 1000000;
        const int64_t t_total_h = t_total_s / 3600;
        t_total_s -= t_total_h * 3600;
        const int64_t t_total_m = t_total_s / 60;
        t_total_s -= t_total_m * 60;
        fprintf(stderr, "%s: training took %02" PRId64 ":%02" PRId64 ":%02" PRId64 "\n", __func__, t_total_h, t_total_m, t_total_s);
    }

    wsp_ggml_opt_free(opt_ctx);
    wsp_ggml_opt_result_free(result_train);
    wsp_ggml_opt_result_free(result_val);
}
