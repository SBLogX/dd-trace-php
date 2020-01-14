<?php

namespace DDTrace\Tests\Common;

use DDTrace\Configuration;
use DDTrace\Tag;

final class SpanAssertion
{
    const NOT_TESTED = '__not_tested__';

    private $operationName;
    private $hasError;
    /** @var string[] Tags the MUST match in both key and value */
    private $exactTags = SpanAssertion::NOT_TESTED;
    /** @var string[] Tags the MUST be present but with any value */
    private $existingTags = [];
    /** @var string[] Ignore any tags that match these regexp patterns */
    private $skipTagPatterns = [];
    /** @var array Exact metrics set on the span */
    private $exactMetrics = SpanAssertion::NOT_TESTED;
    private $service = SpanAssertion::NOT_TESTED;
    private $type = SpanAssertion::NOT_TESTED;
    private $resource = SpanAssertion::NOT_TESTED;
    private $onlyCheckExistence = false;
    private $isTraceAnalyticsCandidate = false;
    /** @var SpanAssertion[] */
    private $children = [];

    private $toBeSkipped = false;

    private $statusCode = SpanAssertion::NOT_TESTED;

    /**
     * @param string $name
     * @param bool $error
     * @param bool $onlyCheckExistance
     */
    private function __construct($name, $error, $onlyCheckExistance)
    {
        $this->operationName = $name;
        $this->hasError = $error;
        $this->onlyCheckExistence = $onlyCheckExistance;
    }

    /**
     * @param string $name
     * @param null $resource
     * @param bool $error
     * @return SpanAssertion
     */
    public static function exists($name, $resource = null, $error = false)
    {
        return SpanAssertion::forOperation($name, $error, true)
            ->resource($resource);
    }

    /**
     * @param string $name
     * @param string $service
     * @param string $type
     * @param string $resource
     * @param array $exactTags
     * @param null $parent
     * @param bool $error
     * @param array|string $extactMetrics
     * @return SpanAssertion
     */
    public static function build(
        $name,
        $service,
        $type,
        $resource,
        $exactTags = [],
        $parent = null,
        $error = false,
        $extactMetrics = SpanAssertion::NOT_TESTED
    ) {
        return SpanAssertion::forOperation($name, $error)
            ->service($service)
            ->resource($resource)
            ->type($type)
            ->withExactTags($exactTags)
            ->withExactMetrics($extactMetrics)
        ;
    }

    /**
     * @param string $name
     * @param bool $error
     * @param bool $onlyCheckExistence
     * @return SpanAssertion
     */
    public static function forOperation($name, $error = false, $onlyCheckExistence = false)
    {
        return new SpanAssertion($name, $error, $onlyCheckExistence);
    }

    /**
     * @param string|null $errorType The expected error.type
     * @param string|null $errorMessage The expected error.msg
     * @param bool $exceptionThrown If we would expect error.stack (sandbox only)
     * @return $this
     */
    public function setError($errorType = null, $errorMessage = null, $exceptionThrown = false)
    {
        $this->hasError = true;
        if (!is_array($this->exactTags)) {
            $this->exactTags = [];
        }
        if (isset($this->exactTags[Tag::ERROR_TYPE])) {
            return $this;
        }
        if (null !== $errorType) {
            $this->exactTags[Tag::ERROR_TYPE] = $errorType;
        } else {
            $this->existingTags[] = Tag::ERROR_TYPE;
        }
        if (null !== $errorMessage) {
            $this->exactTags[Tag::ERROR_MSG] = $errorMessage;
        }
        if ($exceptionThrown && Configuration::get()->isSandboxEnabled()) {
            $this->existingTags[] = Tag::ERROR_STACK;
        }
        return $this;
    }

    /**
     * @param SpanAssertion|SpanAssertion[] $children
     * @return $this
     */
    public function withChildren($children)
    {
        $toBeAdded = is_array($children) ? $children : [$children];
        $this->children = array_merge(
            $this->children,
            array_values(array_filter($toBeAdded, function (SpanAssertion $assertion) {
                return !$assertion->isToBeSkipped();
            }))
        );
        return $this;
    }

    /**
     * @param array|string $tags
     * @return $this
     */
    public function withExactTags($tags)
    {
        if (SpanAssertion::NOT_TESTED === $tags) {
            return $this;
        }

        // At this point, tags and metrics are tested, so they should not be NOT_TESTED anymore.
        $this->exactTags = is_array($this->exactTags) ? $this->exactTags : [];
        $this->exactMetrics = is_array($this->exactMetrics) ? $this->exactMetrics : [];

        foreach ($tags as $name => $value) {
            if ($name === '_sampling_priority_v1') {
                $this->exactMetrics[$name] = $value;
            } elseif (is_int($value) && abs($value) < 9007199254740992) {
                // ints with abs() > 2^53 cannot be correctly converted to float64, which is what
                // the metric array expects.
                $this->exactMetrics[$name] = (float)$value;
            } elseif (\is_float($value)) {
                $this->exactMetrics[$name] = $value;
            } else {
                $this->exactTags[$name] = $value;
            }
        }

        return $this;
    }

    /**
     * @param array|string $metrics
     * @return $this
     */
    public function withExactMetrics($metrics)
    {
        $this->withExactTags($metrics);
        return $this;
    }

    /**
     * @param array $tagNames
     * @param bool $merge If true, the provided tags are nmerged with the default ones
     * @return $this
     */
    public function withExistingTagsNames(array $tagNames, $merge = true)
    {
        $this->existingTags = $merge ? array_merge($tagNames, $this->existingTags) : $tagNames;
        return $this;
    }

    /**
     * @param string $pattern regular expression
     * @return $this
     */
    public function skipTagsLike($pattern)
    {
        $this->skipTagPatterns[] = $pattern;
        return $this;
    }

    public function getSkippedTagPatterns()
    {
        return $this->skipTagPatterns;
    }

    /**
     * @param string $name
     * @return $this
     */
    public function service($name)
    {
        $this->service = $name;
        return $this;
    }

    /**
     * @param string $name
     * @return $this
     */
    public function type($name)
    {
        $this->type = $name;
        return $this;
    }

    /**
     * @param string $name
     * @return $this
     */
    public function resource($name)
    {
        $this->resource = $name;
        return $this;
    }

    /**
     * @return mixed
     */
    public function getOperationName()
    {
        return $this->operationName;
    }

    /**
     * @return SpanAssertion[]
     */
    public function getChildren()
    {
        return $this->children;
    }

    /**
     * @return bool
     */
    public function hasError()
    {
        return $this->hasError;
    }

    /**
     * @return string[]
     */
    public function getExactTags()
    {
        return $this->exactTags;
    }

    /**
     * @param bool $isChildSpan
     * @return string[]
     */
    public function getExistingTagNames($isChildSpan = false)
    {
        if ($isChildSpan) {
            return array_filter($this->existingTags, function ($name) {
                return $name !== 'system.pid';
            });
        }
        return $this->existingTags;
    }

    /**
     * @return int
     */
    public function getStatusCode()
    {
        return $this->statusCode;
    }

    /**
     * @return string
     */
    public function getService()
    {
        return $this->service;
    }

    /**
     * @return string
     */
    public function getType()
    {
        return $this->type;
    }

    /**
     * @return string
     */
    public function getResource()
    {
        return $this->resource;
    }

    /**
     * @return bool
     */
    public function isOnlyCheckExistence()
    {
        return $this->onlyCheckExistence;
    }

    /**
     * @return self
     */
    public function setTraceAnalyticsCandidate()
    {
        $this->isTraceAnalyticsCandidate = true;
        return $this;
    }

    /**
     * @return bool
     */
    public function isTraceAnalyticsCandidate()
    {
        return $this->isTraceAnalyticsCandidate;
    }

    /**
     * @return array
     */
    public function getExactMetrics()
    {
        return $this->exactMetrics;
    }

    public function __toString()
    {
        return sprintf(
            "{name:'%s' resource:'%s'}",
            $this->getOperationName(),
            $this->getResource()
        );
    }

    /**
     * @param $condition
     * @return $this
     */
    public function skipIf($condition)
    {
        $this->toBeSkipped = (bool)$condition;
        return $this;
    }

    /**
     * @param $condition
     * @return $this
     */
    public function onlyIf($condition)
    {
        return $this->skipIf(!$condition);
    }

    /**
     * @return bool
     */
    public function isToBeSkipped()
    {
        return $this->toBeSkipped;
    }
}
