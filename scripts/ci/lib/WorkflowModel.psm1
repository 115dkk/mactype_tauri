# Parses the GitHub Actions YAML subset used by this repository: two-space
# mappings, dash sequences, inline scalar sequences, quoted and plain scalars,
# expression text, comments outside quotes, and literal or folded block scalars
# with optional chomping indicators. Anchors, aliases, tags, flow mappings,
# explicit mapping keys, tabs, and mixed or unexpected indentation are rejected.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function New-WorkflowParseError {
    param(
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [int] $LineNumber,
        [Parameter(Mandatory)] [string] $Message
    )
    throw "Unsupported workflow YAML in '$Path' at line ${LineNumber}: $Message"
}

function Remove-YamlComment {
    param([Parameter(Mandatory)] [string] $Text)

    $single = $false
    $double = $false
    for ($index = 0; $index -lt $Text.Length; $index++) {
        $character = $Text[$index]
        if ($single) {
            if ($character -eq "'") {
                if ($index + 1 -lt $Text.Length -and $Text[$index + 1] -eq "'") {
                    $index++
                } else {
                    $single = $false
                }
            }
            continue
        }
        if ($double) {
            if ($character -eq '\') {
                $index++
            } elseif ($character -eq '"') {
                $double = $false
            }
            continue
        }
        if ($character -eq "'") {
            $single = $true
        } elseif ($character -eq '"') {
            $double = $true
        } elseif ($character -eq '#' -and
            ($index -eq 0 -or [char]::IsWhiteSpace($Text[$index - 1]))) {
            return $Text.Substring(0, $index).TrimEnd()
        }
    }
    return $Text.TrimEnd()
}

function Find-YamlMappingColon {
    param([Parameter(Mandatory)] [string] $Text)

    $single = $false
    $double = $false
    $expressionDepth = 0
    for ($index = 0; $index -lt $Text.Length; $index++) {
        $character = $Text[$index]
        if ($single) {
            if ($character -eq "'") {
                if ($index + 1 -lt $Text.Length -and $Text[$index + 1] -eq "'") {
                    $index++
                } else {
                    $single = $false
                }
            }
            continue
        }
        if ($double) {
            if ($character -eq '\') {
                $index++
            } elseif ($character -eq '"') {
                $double = $false
            }
            continue
        }
        if ($character -eq "'") {
            $single = $true
            continue
        }
        if ($character -eq '"') {
            $double = $true
            continue
        }
        if ($index + 2 -lt $Text.Length -and $Text.Substring($index, 3) -eq '${{') {
            $expressionDepth++
            $index += 2
            continue
        }
        if ($expressionDepth -gt 0 -and $index + 1 -lt $Text.Length -and
            $Text.Substring($index, 2) -eq '}}') {
            $expressionDepth--
            $index++
            continue
        }
        if ($character -eq ':' -and $expressionDepth -eq 0 -and
            ($index + 1 -eq $Text.Length -or [char]::IsWhiteSpace($Text[$index + 1]))) {
            return $index
        }
    }
    return -1
}

function Split-YamlFlowSequence {
    param(
        [Parameter(Mandatory)] [string] $Text,
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [int] $LineNumber
    )

    $items = [System.Collections.Generic.List[string]]::new()
    $start = 0
    $single = $false
    $double = $false
    $expressionDepth = 0
    for ($index = 0; $index -lt $Text.Length; $index++) {
        $character = $Text[$index]
        if ($single) {
            if ($character -eq "'") {
                if ($index + 1 -lt $Text.Length -and $Text[$index + 1] -eq "'") {
                    $index++
                } else {
                    $single = $false
                }
            }
            continue
        }
        if ($double) {
            if ($character -eq '\') {
                $index++
            } elseif ($character -eq '"') {
                $double = $false
            }
            continue
        }
        if ($character -eq "'") {
            $single = $true
            continue
        }
        if ($character -eq '"') {
            $double = $true
            continue
        }
        if ($index + 2 -lt $Text.Length -and $Text.Substring($index, 3) -eq '${{') {
            $expressionDepth++
            $index += 2
            continue
        }
        if ($expressionDepth -gt 0 -and $index + 1 -lt $Text.Length -and
            $Text.Substring($index, 2) -eq '}}') {
            $expressionDepth--
            $index++
            continue
        }
        if ($character -eq ',' -and $expressionDepth -eq 0) {
            $items.Add($Text.Substring($start, $index - $start).Trim())
            $start = $index + 1
        } elseif ($character -in @('[', ']', '{', '}') -and $expressionDepth -eq 0) {
            New-WorkflowParseError -Path $Path -LineNumber $LineNumber `
                -Message 'nested flow collections are not supported'
        }
    }
    if ($single -or $double -or $expressionDepth -ne 0) {
        New-WorkflowParseError -Path $Path -LineNumber $LineNumber `
            -Message 'unterminated quoted scalar or expression'
    }
    $items.Add($Text.Substring($start).Trim())
    return $items.ToArray()
}

function ConvertFrom-YamlScalar {
    param(
        [Parameter(Mandatory)] [string] $Text,
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [int] $LineNumber
    )

    $value = (Remove-YamlComment -Text $Text).Trim()
    if ($value.Length -eq 0) {
        return ''
    }
    if ($value -match '^(?:&|\*|!!|!<|!\w)') {
        New-WorkflowParseError -Path $Path -LineNumber $LineNumber `
            -Message 'anchors, aliases, and tags are not supported'
    }
    if ($value[0] -eq "'") {
        if ($value.Length -lt 2 -or $value[-1] -ne "'") {
            New-WorkflowParseError -Path $Path -LineNumber $LineNumber `
                -Message 'unterminated single-quoted scalar'
        }
        return $value.Substring(1, $value.Length - 2).Replace("''", "'")
    }
    if ($value[0] -eq '"') {
        if ($value.Length -lt 2 -or $value[-1] -ne '"') {
            New-WorkflowParseError -Path $Path -LineNumber $LineNumber `
                -Message 'unterminated double-quoted scalar'
        }
        try {
            return [System.Text.Json.JsonSerializer]::Deserialize(
                $value,
                [string],
                [System.Text.Json.JsonSerializerOptions]::new()
            )
        } catch {
            New-WorkflowParseError -Path $Path -LineNumber $LineNumber `
                -Message "invalid double-quoted scalar: $($_.Exception.Message)"
        }
    }
    if ($value[0] -eq '[') {
        if ($value[-1] -ne ']') {
            New-WorkflowParseError -Path $Path -LineNumber $LineNumber `
                -Message 'unterminated inline sequence'
        }
        $inner = $value.Substring(1, $value.Length - 2).Trim()
        if ($inner.Length -eq 0) {
            return ,@()
        }
        $values = foreach ($item in Split-YamlFlowSequence `
            -Text $inner -Path $Path -LineNumber $LineNumber) {
            ConvertFrom-YamlScalar -Text $item -Path $Path -LineNumber $LineNumber
        }
        return ,@($values)
    }
    if ($value[0] -eq '{') {
        New-WorkflowParseError -Path $Path -LineNumber $LineNumber `
            -Message 'flow mappings are not supported'
    }
    return $value
}

function ConvertFrom-YamlBlockScalar {
    param(
        [Parameter(Mandatory)] [AllowEmptyString()] [string[]] $Lines,
        [Parameter(Mandatory)] [ref] $Index,
        [Parameter(Mandatory)] [int] $ParentIndent,
        [Parameter(Mandatory)] [string] $Indicator,
        [Parameter(Mandatory)] [string] $Path
    )

    if ($Indicator -notmatch '^(?<style>[|>])(?<chomp>[-+]?)$') {
        New-WorkflowParseError -Path $Path -LineNumber ($Index.Value + 1) `
            -Message "unsupported block scalar indicator '$Indicator'"
    }
    $style = $Matches.style
    $chomp = $Matches.chomp
    $content = [System.Collections.Generic.List[object]]::new()
    $contentIndent = $null
    $cursor = $Index.Value + 1
    while ($cursor -lt $Lines.Count) {
        $line = $Lines[$cursor]
        if ($line -match "`t") {
            New-WorkflowParseError -Path $Path -LineNumber ($cursor + 1) `
                -Message 'tabs are not supported'
        }
        $trimmed = $line.TrimStart(' ')
        $indent = $line.Length - $trimmed.Length
        if ($trimmed.Length -gt 0 -and $indent -le $ParentIndent) {
            break
        }
        if ($trimmed.Length -gt 0 -and $null -eq $contentIndent) {
            $contentIndent = $indent
        }
        if ($trimmed.Length -gt 0 -and $indent -lt $contentIndent) {
            New-WorkflowParseError -Path $Path -LineNumber ($cursor + 1) `
                -Message 'block scalar indentation decreased unexpectedly'
        }
        $text = if ($trimmed.Length -eq 0) {
            ''
        } else {
            $line.Substring($contentIndent)
        }
        $content.Add([pscustomobject]@{
            Text = $text
            MoreIndented = $trimmed.Length -gt 0 -and $indent -gt $contentIndent
        })
        $cursor++
    }
    $Index.Value = $cursor - 1
    if ($content.Count -eq 0) {
        return ''
    }
    if ($style -eq '|') {
        $value = (($content | ForEach-Object Text) -join "`n") + "`n"
    } else {
        $builder = [System.Text.StringBuilder]::new()
        for ($itemIndex = 0; $itemIndex -lt $content.Count; $itemIndex++) {
            $item = $content[$itemIndex]
            [void] $builder.Append($item.Text)
            if ($itemIndex + 1 -lt $content.Count) {
                $next = $content[$itemIndex + 1]
                if ($item.Text.Length -eq 0 -or $next.Text.Length -eq 0 -or
                    $item.MoreIndented -or $next.MoreIndented) {
                    [void] $builder.Append("`n")
                } else {
                    [void] $builder.Append(' ')
                }
            }
        }
        [void] $builder.Append("`n")
        $value = $builder.ToString()
    }
    if ($chomp -eq '-') {
        return $value.TrimEnd("`n")
    }
    if ($chomp -ne '+') {
        return $value.TrimEnd("`n") + "`n"
    }
    return $value
}

function ConvertFrom-WorkflowYaml {
    param(
        [Parameter(Mandatory)] [string] $Text,
        [Parameter(Mandatory)] [string] $Path
    )

    $normalized = $Text.Replace("`r`n", "`n").Replace("`r", "`n")
    [string[]] $lines = $normalized.Split("`n")
    $root = [ordered]@{}
    $stack = [System.Collections.Generic.Stack[object]]::new()
    $stack.Push([pscustomobject]@{
        Indent = -2
        Type = 'map'
        Value = $root
        Pending = $null
    })

    for ($lineIndex = 0; $lineIndex -lt $lines.Count; $lineIndex++) {
        $line = $lines[$lineIndex]
        if ($line -match "`t") {
            New-WorkflowParseError -Path $Path -LineNumber ($lineIndex + 1) `
                -Message 'tabs are not supported'
        }
        $trimmed = $line.TrimStart(' ')
        if ($trimmed.Length -eq 0 -or $trimmed.StartsWith('#')) {
            continue
        }
        $indent = $line.Length - $trimmed.Length
        if ($indent % 2 -ne 0) {
            New-WorkflowParseError -Path $Path -LineNumber ($lineIndex + 1) `
                -Message 'indentation must use two-space increments'
        }
        while ($stack.Count -gt 1 -and $indent -le $stack.Peek().Indent) {
            $completedFrame = $stack.Pop()
            if ($completedFrame.Pending) {
                $completedFrame.Value[$completedFrame.Pending] = $null
                $completedFrame.Pending = $null
            }
        }
        $topFrame = $stack.Peek()
        if ($topFrame.Pending -and $indent -le $topFrame.Indent + 2) {
            $topFrame.Value[$topFrame.Pending] = $null
            $topFrame.Pending = $null
        }
        $parent = $stack.Peek()
        if ($parent.Pending) {
            if ($indent -le $parent.Indent + 2) {
                $parent.Value[$parent.Pending] = $null
                $parent.Pending = $null
            }
        }
        if ($parent.Pending) {
            if ($trimmed -eq '-' -or $trimmed.StartsWith('- ')) {
                $container = [System.Collections.Generic.List[object]]::new()
            } else {
                $container = [ordered]@{}
            }
            $pendingKey = $parent.Pending
            $parent.Value[$pendingKey] = $container
            $parent.Pending = $null
            $container = $parent.Value[$pendingKey]
            $containerType = if ($trimmed -eq '-' -or $trimmed.StartsWith('- ')) {
                'sequence'
            } else {
                'map'
            }
            $containerFrame = [pscustomobject]@{
                Indent = $indent - 2
                Type = $containerType
                Value = $container
                Pending = $null
            }
            $stack.Push($containerFrame)
            $parent = $containerFrame
        }
        if ($indent -ne $parent.Indent + 2) {
            New-WorkflowParseError -Path $Path -LineNumber ($lineIndex + 1) `
                -Message 'unexpected indentation'
        }

        if ($trimmed -eq '-' -or $trimmed.StartsWith('- ')) {
            if ($parent.Type -ne 'sequence') {
                New-WorkflowParseError -Path $Path -LineNumber ($lineIndex + 1) `
                    -Message 'sequence item appears outside a sequence'
            }
            $remainder = if ($trimmed.Length -eq 1) { '' } else { $trimmed.Substring(2) }
            if ($remainder.Length -eq 0) {
                $item = [ordered]@{}
                $parent.Value.Add($item)
                $stack.Push([pscustomobject]@{
                    Indent = $indent
                    Type = 'map'
                    Value = $item
                    Pending = $null
                })
                continue
            }
            $colon = Find-YamlMappingColon -Text $remainder
            if ($colon -lt 0) {
                $parent.Value.Add((ConvertFrom-YamlScalar `
                    -Text $remainder -Path $Path -LineNumber ($lineIndex + 1)))
                continue
            }
            $key = $remainder.Substring(0, $colon).Trim()
            if ($key.Length -eq 0 -or $key[0] -in @('?', '[', '{')) {
                New-WorkflowParseError -Path $Path -LineNumber ($lineIndex + 1) `
                    -Message 'unsupported mapping key'
            }
            $item = [ordered]@{}
            if ($null -eq $parent.Value) {
                $propertyDump = $parent.PSObject.Properties | ForEach-Object { "$($_.Name)=$($_.Value)" }
                throw "Internal workflow parser state lost its sequence at line $($lineIndex + 1) (frame $($parent.Type), indent $($parent.Indent), $($propertyDump -join '; '))."
            }
            $parent.Value.Add($item)
            $valueText = $remainder.Substring($colon + 1).Trim()
            $frame = [pscustomobject]@{
                Indent = $indent
                Type = 'map'
                Value = $item
                Pending = $null
            }
            if ($valueText.Length -eq 0) {
                $item[$key] = $null
                $frame.Pending = $key
            } elseif ($valueText -match '^[|>]') {
                $item[$key] = ConvertFrom-YamlBlockScalar `
                    -Lines $lines -Index ([ref] $lineIndex) -ParentIndent $indent `
                    -Indicator $valueText -Path $Path
            } else {
                $item[$key] = ConvertFrom-YamlScalar `
                    -Text $valueText -Path $Path -LineNumber ($lineIndex + 1)
            }
            $stack.Push($frame)
            continue
        }

        if ($parent.Type -ne 'map') {
            New-WorkflowParseError -Path $Path -LineNumber ($lineIndex + 1) `
                -Message 'mapping entry appears inside a scalar sequence'
        }
        $colon = Find-YamlMappingColon -Text $trimmed
        if ($colon -lt 1) {
            New-WorkflowParseError -Path $Path -LineNumber ($lineIndex + 1) `
                -Message 'expected a mapping entry'
        }
        $key = $trimmed.Substring(0, $colon).Trim()
        if ($key[0] -in @('?', '[', '{') -or $key -match '^(?:&|\*|!!|!)' -or $key -match '(?<![A-Za-z0-9_])(?:&|\*)[A-Za-z0-9_-]+') {
            New-WorkflowParseError -Path $Path -LineNumber ($lineIndex + 1) `
                -Message 'unsupported mapping key'
        }
        if ($parent.Value.Contains($key)) {
            New-WorkflowParseError -Path $Path -LineNumber ($lineIndex + 1) `
                -Message "duplicate mapping key '$key'"
        }
        $valueText = $trimmed.Substring($colon + 1).Trim()
        if ($valueText.Length -eq 0) {
            $parent.Value[$key] = $null
            $parent.Pending = $key
        } elseif ($valueText -match '^[|>]') {
            $parent.Value[$key] = ConvertFrom-YamlBlockScalar `
                -Lines $lines -Index ([ref] $lineIndex) -ParentIndent $indent `
                -Indicator $valueText -Path $Path
        } else {
            $parent.Value[$key] = ConvertFrom-YamlScalar `
                -Text $valueText -Path $Path -LineNumber ($lineIndex + 1)
        }
    }
    foreach ($frame in $stack) {
        if ($frame.Pending) {
            $frame.Value[$frame.Pending] = $null
            $frame.Pending = $null
        }
    }
    return $root
}

function Get-MapValue {
    param(
        [Parameter(Mandatory)] [System.Collections.IDictionary] $Map,
        [Parameter(Mandatory)] [string] $Key,
        $Default = $null
    )
    if ($Map.Contains($Key)) {
        return $Map[$Key]
    }
    return $Default
}

function ConvertTo-WorkflowHashtable {
    param($Value)

    $result = @{}
    if ($null -eq $Value -or $Value -isnot [System.Collections.IDictionary]) {
        return $result
    }
    foreach ($key in $Value.Keys) {
        $result[[string] $key] = $Value[$key]
    }
    return $result
}

function ConvertTo-WorkflowStringArray {
    param($Value)

    if ($null -eq $Value -or $Value -is [System.Collections.IDictionary]) {
        return [string[]] @()
    }
    if ($Value -is [System.Collections.IList] -and $Value -isnot [string]) {
        return [string[]] @($Value | ForEach-Object { [string] $_ })
    }
    return [string[]] @([string] $Value)
}

function ConvertTo-WorkflowStep {
    param([Parameter(Mandatory)] [System.Collections.IDictionary] $Step)

    return [pscustomobject]@{
        Name = [string] (Get-MapValue -Map $Step -Key 'name' -Default '')
        Id = [string] (Get-MapValue -Map $Step -Key 'id' -Default '')
        Uses = [string] (Get-MapValue -Map $Step -Key 'uses' -Default '')
        Run = [string] (Get-MapValue -Map $Step -Key 'run' -Default '')
        Shell = [string] (Get-MapValue -Map $Step -Key 'shell' -Default '')
        WorkingDirectory = [string] (Get-MapValue `
            -Map $Step -Key 'working-directory' -Default '')
        If = [string] (Get-MapValue -Map $Step -Key 'if' -Default '')
        With = ConvertTo-WorkflowHashtable (Get-MapValue -Map $Step -Key 'with')
        Env = ConvertTo-WorkflowHashtable (Get-MapValue -Map $Step -Key 'env')
    }
}

function Get-TopLevelRawBlock {
    param(
        [Parameter(Mandatory)] [string] $Text,
        [Parameter(Mandatory)] [string] $Key
    )

    $normalized = $Text.Replace("`r`n", "`n").Replace("`r", "`n")
    [string[]] $lines = $normalized.Split("`n")
    $start = -1
    for ($index = 0; $index -lt $lines.Count; $index++) {
        if ($lines[$index] -match "^$([regex]::Escape($Key)):\s*(?:#.*)?$") {
            $start = $index
            break
        }
    }
    if ($start -lt 0) {
        return ''
    }
    $end = $lines.Count
    for ($index = $start + 1; $index -lt $lines.Count; $index++) {
        if ($lines[$index] -match '^[A-Za-z0-9_-]+:\s*') {
            $end = $index
            break
        }
    }
    return ($lines[$start..($end - 1)] -join "`n").TrimEnd("`n")
}

function Read-GitHubWorkflow {
    [CmdletBinding()]
    param([Parameter(Mandatory)] [string] $Path)

    $resolvedPath = (Resolve-Path -LiteralPath $Path).Path
    $rawText = [System.IO.File]::ReadAllText($resolvedPath)
    $document = ConvertFrom-WorkflowYaml -Text $rawText -Path $resolvedPath
    foreach ($requiredKey in @('name', 'on', 'jobs')) {
        if (-not $document.Contains($requiredKey)) {
            throw "Workflow '$resolvedPath' is missing top-level '$requiredKey'."
        }
    }
    if ($document.jobs -isnot [System.Collections.IDictionary]) {
        throw "Workflow '$resolvedPath' has a non-mapping jobs value."
    }
    $jobs = [ordered]@{}
    foreach ($jobId in $document.jobs.Keys) {
        $job = $document.jobs[$jobId]
        if ($job -isnot [System.Collections.IDictionary]) {
            throw "Workflow '$resolvedPath' job '$jobId' is not a mapping."
        }
        $steps = [System.Collections.Generic.List[object]]::new()
        $stepValues = $job['steps']
        if (($stepValues -is [System.Collections.IEnumerable]) -and
            ($stepValues -isnot [System.Collections.IDictionary]) -and
            ($stepValues -isnot [string])) {
            foreach ($step in $stepValues) {
                if ($step -isnot [System.Collections.IDictionary]) {
                    throw "Workflow '$resolvedPath' job '$jobId' contains a non-mapping step."
                }
                $steps.Add((ConvertTo-WorkflowStep -Step $step))
            }
        } elseif ($null -ne $stepValues) {
            $jobDump = $job.Keys | ForEach-Object {
                $value = $job[$_]
                "$($_)=$($value.GetType().Name):$value"
            }
            throw "Workflow '$resolvedPath' job '$jobId' has a non-sequence steps value ($($stepValues.GetType().FullName): $stepValues). Job: $($jobDump -join '; ')"
        }
        $jobs[[string] $jobId] = [pscustomobject]@{
            Id = [string] $jobId
            Name = [string] (Get-MapValue -Map $job -Key 'name' -Default '')
            RunsOn = Get-MapValue -Map $job -Key 'runs-on'
            Needs = ConvertTo-WorkflowStringArray (Get-MapValue -Map $job -Key 'needs')
            If = [string] (Get-MapValue -Map $job -Key 'if' -Default '')
            Environment = Get-MapValue -Map $job -Key 'environment'
            Permissions = Get-MapValue -Map $job -Key 'permissions'
            Steps = $steps
        }
    }
    $workflow = [pscustomobject]@{
        Name = [string] $document.name
        On = Get-TopLevelRawBlock -Text $rawText -Key 'on'
        Env = ConvertTo-WorkflowHashtable (Get-MapValue -Map $document -Key 'env')
        Jobs = $jobs
    }
    $workflow.PSObject.Properties.Add([psnoteproperty]::new('__RawText', $rawText))
    return $workflow
}

function Get-WorkflowJob {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] $Workflow,
        [Parameter(Mandatory)] [string] $Id
    )

    if ($Workflow.Jobs.Contains($Id)) {
        return $Workflow.Jobs[$Id]
    }
    return $null
}

function Get-WorkflowStep {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] $Job,
        [string] $NameLike,
        [string] $RunLike,
        [string] $Uses
    )

    $matches = @($Job.Steps)
    if ($PSBoundParameters.ContainsKey('NameLike')) {
        $matches = @($matches | Where-Object Name -Like $NameLike)
    }
    if ($PSBoundParameters.ContainsKey('RunLike')) {
        $matches = @($matches | Where-Object Run -Like $RunLike)
    }
    if ($PSBoundParameters.ContainsKey('Uses')) {
        $matches = @($matches | Where-Object Uses -EQ $Uses)
    }
    return $matches
}

function Get-WorkflowRawText {
    [CmdletBinding()]
    param([Parameter(Mandatory)] $Workflow)

    return [string] $Workflow.__RawText
}

Export-ModuleMember -Function @(
    'Read-GitHubWorkflow',
    'Get-WorkflowJob',
    'Get-WorkflowStep',
    'Get-WorkflowRawText'
)
