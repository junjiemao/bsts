#include "state_space_logit_model_manager.h"
#include "utils.h"
#include "r_interface/prior_specification.hpp"
#include "Models/StateSpace/PosteriorSamplers/StateSpaceLogitPosteriorSampler.hpp"
#include "Models/Glm/PosteriorSamplers/BinomialLogitSpikeSlabSampler.hpp"
#include "Models/MvnModel.hpp"
#include "Models/Glm/VariableSelectionPrior.hpp"

namespace BOOM {
namespace bsts {

StateSpaceLogitModelManager::StateSpaceLogitModelManager()
    : predictor_dimension_(-1),
      clt_threshold_(5) {}

StateSpaceLogitModel * StateSpaceLogitModelManager::CreateObservationModel(
    SEXP r_data_list,
    SEXP r_prior,
    SEXP r_options,
    RListIoManager *io_manager) {

  if (!Rf_isNull(r_data_list)) {
    // If we were passed data from R then use it to build the model.
    bool regression = !Rf_isNull(getListElement(r_data_list, "predictors"));
    Vector successes(ToBoomVector(getListElement(r_data_list, "response")));
    Vector trials(ToBoomVector(getListElement(r_data_list, "trials")));
    // If there are no predictors then make an intercept.
    Matrix predictors =
        regression ?
        ToBoomMatrix(getListElement(r_data_list, "predictors")) :
        Matrix(successes.size(), 1, 1.0);
    std::vector<bool> response_is_observed(ToVectorBool(getListElement(
        r_data_list, "response.is.observed")));

    model_.reset(new StateSpaceLogitModel(
      successes,
      trials,
      predictors,
      response_is_observed));
    // With the Gaussian models we have two separate classes for the
    // regression and non-regression cases.  For non-Gaussian models
    // we have a single class with a regression bit that can be
    // turned off.
    model_->set_regression_flag(regression);
  } else {
    // If no data was passed from R then build the model from its
    // default constructor.  We need to know the dimension of the
    // predictors.
    if (predictor_dimension_ < 0) {
      report_error("If r_data_list is NULL then you must call "
                   "SetPredictorDimension before creating a model.");
    }
    model_.reset(new StateSpaceLogitModel(predictor_dimension_));
  }

  // Set the CLT threshold for data imputation.  If not supplied use 5
  // (from the constructor) as a default.  The CLT threshold argument
  // is documented in the source for the BinomialLogitAuxMixSampler
  // constructor (which is a base class for
  // BinomialLogitSpikeSlabSampler).
  if (!Rf_isNull(r_options)) {
    SEXP r_clt_threshold = getListElement(r_options, "clt.threshold");
    if (!Rf_isNull(r_clt_threshold)) {
      clt_threshold_ = Rf_asInteger(r_clt_threshold);
    }
  }

  Ptr<BinomialLogitSpikeSlabSampler> observation_model_sampler;
  if (!Rf_isNull(r_prior)
      && Rf_inherits(r_prior, "SpikeSlabPriorBase")) {
    // If r_prior is NULL it could either mean that there are no
    // predictors, or that an existing model is being reinstantiated.
    RInterface::SpikeSlabGlmPrior prior_spec(r_prior);
    observation_model_sampler = new BinomialLogitSpikeSlabSampler(
        model_->observation_model(),
        prior_spec.slab(),
        prior_spec.spike(),
        clt_threshold_);  // see above for clt_threshold_
    DropUnforcedCoefficients(
        model_->observation_model(),
        prior_spec.spike()->prior_inclusion_probabilities());
    // Restrict number of model selection sweeps if max_flips was set.
    int max_flips = prior_spec.max_flips();
    if (max_flips > 0) {
      observation_model_sampler->limit_model_selection(max_flips);
    }
    // Make the io_manager aware of the model parameters.
    io_manager->add_list_element(
        new GlmCoefsListElement(model_->observation_model()->coef_prm(),
                                "coefficients"));
  } else {
    // In the non-regression (or no sampler necessary) case make a
    // spike and slab prior that never includes anything.
    observation_model_sampler = new BinomialLogitSpikeSlabSampler(
        model_->observation_model(),
        new MvnModel(1),
        new VariableSelectionPrior(Vector(1, 0.0)),
        clt_threshold_);
  }
  // Both the observation_model and the actual model_ need to have
  // their posterior samplers set.
  model_->observation_model()->set_method(observation_model_sampler);
  Ptr<StateSpaceLogitPosteriorSampler> sampler(
      new StateSpaceLogitPosteriorSampler(
          model_.get(),
          observation_model_sampler));
  model_->set_method(sampler);
  return model_.get();
}

void StateSpaceLogitModelManager::SetPredictorDimension(int xdim) {
  predictor_dimension_ = xdim;
}

void StateSpaceLogitModelManager::AddDataFromBstsObject(SEXP r_bsts_object) {
  Vector successes = ToBoomVector(getListElement(
      r_bsts_object, "original.series"));
  AddData(successes,
          ToBoomVector(getListElement(r_bsts_object, "trials")),
          ExtractPredictors(r_bsts_object, "predictors", successes.size()),
          IsObserved(getListElement(r_bsts_object, "original.series")));
}

void StateSpaceLogitModelManager::AddDataFromList(
    SEXP r_data_list) {
  Vector successes = ToBoomVector(getListElement(r_data_list, "response"));
  AddData(successes,
          ToBoomVector(getListElement(r_data_list, "trials")),
          ExtractPredictors(r_data_list, "predictors", successes.size()),
          ToVectorBool(getListElement(r_data_list, "response.is.observed")));
}

int StateSpaceLogitModelManager::UnpackForecastData(SEXP r_prediction_data) {
  forecast_trials_ = ToBoomVector(getListElement(r_prediction_data, "trials"));
  int forecast_horizon = forecast_trials_.size();
  forecast_predictors_ = ExtractPredictors(r_prediction_data, "predictors",
                                           forecast_horizon);
  return forecast_horizon;
}

Vector StateSpaceLogitModelManager::SimulateForecast(
    const Vector &final_state) {
  return model_->simulate_forecast(forecast_predictors_,
                                   forecast_trials_,
                                   final_state);
}

int StateSpaceLogitModelManager::UnpackHoldoutData(SEXP r_holdout_data) {
  forecast_trials_ = ToBoomVector(getListElement(r_holdout_data, "trials"));
  forecast_predictors_ = ExtractPredictors(r_holdout_data, "predictors",
                                           forecast_trials_.size());
  holdout_response_ = ToBoomVector(getListElement(r_holdout_data, "response"));
  int n = holdout_response_.size();
  if (forecast_predictors_.nrow() != n || holdout_response_.size() != n) {
    report_error("Holdout data of the wrong size passed to "
                 "StateSpaceLogitModelManager::UpackHoldoutData");
  }
  return n;
}

Vector StateSpaceLogitModelManager::HoldoutDataOneStepHoldoutPredictionErrors(
    const Vector &final_state) {
  BinomialLogitCltDataImputer data_imputer(clt_threshold_);
  return model_->one_step_holdout_prediction_errors(
      GlobalRng::rng,
      data_imputer,
      holdout_response_,
      forecast_trials_,
      forecast_predictors_,
      final_state);
}

void StateSpaceLogitModelManager::AddData(
    const Vector &successes,
    const Vector &trials,
    const Matrix &predictors,
    const std::vector<bool> &response_is_observed) {
  for (int i = 0; i < successes.size(); ++i) {
    Ptr<StateSpace::AugmentedBinomialRegressionData> data_point(
        new StateSpace::AugmentedBinomialRegressionData(
            successes[i],
            trials[i],
            predictors.row(i)));
    if (!response_is_observed[i]) {
      data_point->set_missing_status(Data::missing_status::completely_missing);
    }
    model_->add_data(data_point);
  }
}

}  // namespace bsts
}  // namespace BOOM
