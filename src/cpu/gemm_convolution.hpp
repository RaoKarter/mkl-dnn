/*******************************************************************************
* Copyright 2016-2018 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef CPU_JIT_GEMM_CONVOLUTION_HPP
#define CPU_JIT_GEMM_CONVOLUTION_HPP

#include "c_types_map.hpp"
#include "cpu_convolution_pd.hpp"
#include "cpu_engine.hpp"
#include "gemm_convolution_utils.hpp"
#include "gemm/gemm.hpp"

namespace mkldnn {
namespace impl {
namespace cpu {

template <bool with_relu>
struct _gemm_convolution_fwd_t: public cpu_primitive_t {
    struct pd_t: public _cpu_convolution_fwd_pd_t<with_relu> {
        pd_t(engine_t *engine,
                const typename pd_t::base_desc_t *adesc,
                const primitive_attr_t *attr,
                const typename pd_t::base_class *hint_fwd_pd)
            : _cpu_convolution_fwd_pd_t<with_relu>(engine, adesc, attr,
                    hint_fwd_pd)
            , jcp_() {}

        DECLARE_COMMON_PD_T(GEMM_IMPL_STR, _gemm_convolution_fwd_t<with_relu>);

        inline memory_format_t src_format()
        {
            using namespace memory_format;
            return (this->cdesc_().src_desc.ndims == 4) ? nchw : ncdhw;
        }
        inline memory_format_t wei_format()
        {
            using namespace memory_format;
            return (this->cdesc_().src_desc.ndims == 4)
                ? this->with_groups() ? goihw : oihw
                : this->with_groups() ? goidhw : oidhw;
        }

        virtual status_t init() override {
            using namespace prop_kind;
            using namespace memory_format;

            assert(this->engine()->kind() == engine_kind::cpu);

            bool ok = true
                && this->set_default_params() == status::success
                && utils::one_of(this->cdesc_().prop_kind, forward_training,
                           forward_inference)
                && this->cdesc_().alg_kind == alg_kind::convolution_direct
                && utils::everyone_is(data_type::f32,
                           this->cdesc_().src_desc.data_type,
                           this->cdesc_().weights_desc.data_type,
                           this->cdesc_().dst_desc.data_type)
                && utils::implication(this->with_bias(), data_type::f32
                                   == this->cdesc_().bias_desc.data_type)
                && this->src_pd_.desc()->format == src_format()
                && this->dst_pd_.desc()->format == src_format()
                && this->weights_pd_.desc()->format == wei_format()
                && this->is_gemm_conv_format();
            return ok ? status::success : status::unimplemented;
        }

        jit_gemm_conv_conf_t jcp_;

    protected:
        virtual status_t set_default_params() override {
            using namespace memory_format;
            if (this->src_pd_.desc()->format == any)
                CHECK(this->src_pd_.set_format(src_format()));
            if (this->dst_pd_.desc()->format == any)
                CHECK(this->dst_pd_.set_format(src_format()));
            if (this->weights_pd_.desc()->format == any)
                CHECK(this->weights_pd_.set_format(wei_format()));
            if (this->bias_pd_.desc()->format == any)
                CHECK(this->bias_pd_.set_format(x));
            return status::success;
        }

        virtual bool is_gemm_conv_format() const {
            bool ok = true;
            auto const &po = this->attr()->post_ops_;
            switch (po.len_) {
                using namespace mkldnn::impl::primitive_kind;
            case 0: // no post_ops
                break;
            case 1:
                ok = ok && // sum OR relu
                        (po.entry_[0].is_relu() || po.entry_[0].is_sum());
                break;
            case 2:
                ok = ok && // sum->relu
                        (po.entry_[0].is_sum() && po.entry_[1].is_relu());
                break;
            default: ok = false;
            }
            return ok;
        }
    };

    _gemm_convolution_fwd_t(const pd_t *pd, const input_vector &inputs,
           const output_vector &outputs)
        : cpu_primitive_t(&conf_, inputs, outputs), conf_(*pd)
        , col_(nullptr)
    {
        using namespace prop_kind;

        const auto &post_ops = conf_.attr()->post_ops_;
        const data_t one = 1.0, zero = 0.0;
        beta_ = post_ops.find(primitive_kind::sum) >= 0 ? one : zero;

        jit_gemm_convolution_utils::init_conf(conf_.jcp_,
            *(conf_.cdesc()), conf_.src_pd(), conf_.weights_pd(0),
            conf_.dst_pd(), with_relu, conf_.negative_slope());

        nthr_ = this->conf_.jcp_.os / omp_get_max_threads() < 512 &&
                utils::implication(this->conf_.jcp_.od == 1,
                (this->conf_.jcp_.mb != 1 || this->conf_.jcp_.ngroups > 2)) ?
                omp_get_max_threads() : 1;

        jit_gemm_convolution_utils::prepare_ws_col<data_t>(this->conf_.jcp_,
                &this->col_, nthr_);
    }

    ~_gemm_convolution_fwd_t() {
        free(this->col_);
    };

    typedef typename prec_traits<data_type::f32>::type data_t;

    virtual void execute(event_t *e) {
        execute_forward();
        e->set_state(event_t::ready);
    }

private:
    void execute_forward();
    pd_t conf_;
    data_t *col_;
    data_t beta_;
    int nthr_;
};

using gemm_convolution_fwd_t =
                         _gemm_convolution_fwd_t<false>;
using gemm_convolution_relu_t =
                         _gemm_convolution_fwd_t<true>;

struct gemm_convolution_bwd_data_t: public cpu_primitive_t {
    struct pd_t: public cpu_convolution_bwd_data_pd_t {
        pd_t(engine_t *engine,
                const convolution_desc_t *adesc,
                const primitive_attr_t *attr,
                const convolution_fwd_pd_t *hint_fwd_pd)
            : cpu_convolution_bwd_data_pd_t(engine, adesc, attr, hint_fwd_pd)
            , jcp_()
        {}

        DECLARE_COMMON_PD_T(GEMM_IMPL_STR, gemm_convolution_bwd_data_t);

        inline memory_format_t src_format()
        {
            using namespace memory_format;
            return (this->desc()->diff_src_desc.ndims == 4) ? nchw : ncdhw;
        }
        inline memory_format_t wei_format()
        {
            using namespace memory_format;
            return (this->desc()->diff_src_desc.ndims == 4)
                ? this->with_groups() ? goihw : oihw
                : this->with_groups() ? goidhw : oidhw;
        }

        virtual status_t init() override {
            using namespace prop_kind;
            using namespace memory_format;

            assert(this->engine()->kind() == engine_kind::cpu);

            bool ok = true
                && this->set_default_params() == status::success
                && utils::one_of(this->desc()->prop_kind, backward,
                        backward_data)
                && this->desc()->alg_kind == alg_kind::convolution_direct
                && utils::everyone_is(data_type::f32,
                        this->desc()->diff_src_desc.data_type,
                        this->desc()->weights_desc.data_type,
                        this->desc()->diff_dst_desc.data_type)
                && this->diff_src_pd_.desc()->format == src_format()
                && this->diff_dst_pd_.desc()->format == src_format()
                && this->weights_pd_.desc()->format == wei_format();
            return ok ? status::success : status::unimplemented;
        }

        jit_gemm_conv_conf_t jcp_;

    protected:
        virtual status_t set_default_params() override {
            using namespace memory_format;
            if (this->diff_src_pd_.desc()->format == any)
                CHECK(this->diff_src_pd_.set_format(src_format()));
            if (this->diff_dst_pd_.desc()->format == any)
                CHECK(this->diff_dst_pd_.set_format(src_format()));
            if (this->weights_pd_.desc()->format == any)
                CHECK(this->weights_pd_.set_format(wei_format()));
            return status::success;
        }
    };

    gemm_convolution_bwd_data_t(const pd_t *pd, const input_vector &inputs,
              const output_vector &outputs)
        : cpu_primitive_t(&conf_, inputs, outputs), conf_(*pd)
        , col_(nullptr)
    {
        using namespace prop_kind;

        jit_gemm_convolution_utils::init_conf(conf_.jcp_,
            *(conf_.desc()), conf_.diff_src_pd(), conf_.weights_pd(0),
            conf_.diff_dst_pd());

        nthr_ = this->conf_.jcp_.mb != 1 || this->conf_.jcp_.ngroups > 2 ?
                omp_get_max_threads() : 1;

        jit_gemm_convolution_utils::prepare_ws_col<data_t>(this->conf_.jcp_,
                &this->col_, nthr_);
    }

    ~gemm_convolution_bwd_data_t() {
        free(this->col_);
    };

    typedef typename prec_traits<data_type::f32>::type data_t;

    virtual void execute(event_t *e) {
        switch (conf_.desc()->prop_kind) {
        case prop_kind::backward:
        case prop_kind::backward_data:
            execute_backward_data();
            break;
        default:
            assert(!"invalid prop_kind");
        }
        e->set_state(event_t::ready);
    }

private:
    void execute_backward_data();
    pd_t conf_;
    data_t *col_;
    int nthr_;
};

struct gemm_convolution_bwd_weights_t: public cpu_primitive_t {
    struct pd_t: public cpu_convolution_bwd_weights_pd_t {
        pd_t(engine_t *engine,
                const convolution_desc_t *adesc,
                const primitive_attr_t *attr,
                const convolution_fwd_pd_t *hint_fwd_pd)
            : cpu_convolution_bwd_weights_pd_t(engine, adesc, attr, hint_fwd_pd)
            , jcp_()
        {}

        DECLARE_COMMON_PD_T(GEMM_IMPL_STR, gemm_convolution_bwd_weights_t);

        inline memory_format_t src_format()
        {
            using namespace memory_format;
            return (this->desc()->src_desc.ndims == 4) ? nchw : ncdhw;
        }
        inline memory_format_t wei_format()
        {
            using namespace memory_format;
            return (this->desc()->src_desc.ndims == 4)
                ? this->with_groups() ? goihw : oihw
                : this->with_groups() ? goidhw : oidhw;
        }

        virtual status_t init() override {
            using namespace prop_kind;
            using namespace memory_format;

            assert(this->engine()->kind() == engine_kind::cpu);

            bool ok = true
            && this->set_default_params() == status::success
            && utils::one_of(this->desc()->prop_kind, backward,
                    backward_weights)
            && this->desc()->alg_kind == alg_kind::convolution_direct
            && utils::everyone_is(data_type::f32,
                    this->desc()->src_desc.data_type,
                    this->desc()->diff_weights_desc.data_type,
                    this->desc()->diff_dst_desc.data_type)
            && utils::implication(this->with_bias(),
                    data_type::f32 == this->desc()->diff_bias_desc.data_type)
            && this->src_pd_.desc()->format == src_format()
            && this->diff_dst_pd_.desc()->format == src_format()
            && this->diff_weights_pd_.desc()->format == wei_format();
            return ok ? status::success : status::unimplemented;
        }

        jit_gemm_conv_conf_t jcp_;

    protected:
        virtual status_t set_default_params() override {
            using namespace memory_format;
            if (this->src_pd_.desc()->format == any)
                CHECK(this->src_pd_.set_format(src_format()));
            if (this->diff_dst_pd_.desc()->format == any)
                CHECK(this->diff_dst_pd_.set_format(src_format()));
            if (this->diff_weights_pd_.desc()->format == any)
                CHECK(this->diff_weights_pd_.set_format(wei_format()));
            if (this->diff_bias_pd_.desc()->format == any)
                CHECK(this->diff_bias_pd_.set_format(x));
            return status::success;
        }
    };

    gemm_convolution_bwd_weights_t(const pd_t *pd, const input_vector &inputs,
              const output_vector &outputs)
        : cpu_primitive_t(&conf_, inputs, outputs), conf_(*pd)
        , col_(nullptr), wei_reduction_(nullptr)
    {
        using namespace prop_kind;

        jit_gemm_convolution_utils::init_conf(conf_.jcp_,
            *(conf_.desc()), conf_.src_pd(), conf_.diff_weights_pd(0),
            conf_.diff_dst_pd());
        const memory_desc_wrapper weights_d(conf_.diff_weights_pd(0));

        nthr_ = this->conf_.jcp_.os / omp_get_max_threads() < 256 &&
                (this->conf_.jcp_.mb != 1 || this->conf_.jcp_.ngroups > 2) ?
                omp_get_max_threads() : 1;

        jit_gemm_convolution_utils::prepare_ws_col<data_t>(this->conf_.jcp_,
                &this->col_, nthr_);
        jit_gemm_convolution_utils::prepare_ws_wei_reduction(this->conf_.jcp_,
                &this->wei_reduction_, weights_d.size(), nthr_);
    }

    ~gemm_convolution_bwd_weights_t() {
        free(this->col_);
        free(this->wei_reduction_);
     };

    typedef typename prec_traits<data_type::f32>::type data_t;

    virtual void execute(event_t *e) {
        switch (conf_.desc()->prop_kind) {
        case prop_kind::backward:
        case prop_kind::backward_weights:
            execute_backward_weights();
            break;
        default:
            assert(!"invalid prop_kind");
        }
        e->set_state(event_t::ready);
    }

private:
    void execute_backward_weights();
    pd_t conf_;
    data_t *col_, *wei_reduction_;
    int nthr_;
};

}
}
}

#endif
